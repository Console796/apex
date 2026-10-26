#!/usr/bin/env python3
"""Apex release packaging (plan 0.1.10).

    tools/package.py [--build-dir DIR] [--out DIR] [--no-bundle-qt] [--qt-prefix DIR]

Takes an existing build tree (default build/linux-gcc-release, or build/windows-msvc-release on
Windows) of the `apex` target, runs `cmake --install` into a staging directory, adds the Qt
runtime, and writes the archive into --out (default dist/):

  Linux:   apex-<version>-linux-x86_64.tar.gz
             apex-<version>/apex            launcher script (sets LD_LIBRARY_PATH, QT_PLUGIN_PATH)
             apex-<version>/bin/apex        the executable
             apex-<version>/lib/            Qt libraries (ldd closure minus glibc/GPU/system libs)
             apex-<version>/plugins/        Qt platform plugins
             apex-<version>/share/doc/apex/ LICENSE, THIRD_PARTY.md
  Windows: apex-<version>-windows-x64.zip
             apex-<version>/apex.exe + windeployqt output, LICENSE, THIRD_PARTY.md

An AppImage is not produced: it needs appimagetool plus a patched Qt, which is not trivial
(plan 0.1.10 allows skipping it). The Vulkan loader and GPU drivers always come from the system.
The version comes from version.txt plus the build number (0 for ad-hoc packages).
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Libraries that must come from the host, not the bundle.
SYSTEM_LIB = re.compile(
    r"^(linux-vdso|ld-linux|libc\.|libm\.|libdl|libpthread|librt|libutil|libresolv|libgcc_s|"
    r"libstdc\+\+|libvulkan|libGL|libEGL|libGLX|libOpenGL|libX|libxcb|libxkbcommon|libwayland|"
    r"libdrm|libgbm|libfontconfig|libfreetype|libharfbuzz|libdbus|libglib|libgobject|libgio|"
    r"libgmodule|libsystemd|libz\.|libasound|libpulse)")

LAUNCHER = """#!/bin/sh
here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$here/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
exec "$here/bin/apex" "$@"
"""


def run(cmd, **kw):
    print("+", " ".join(map(str, cmd)))
    subprocess.run(cmd, check=True, **kw)


def version(build_dir: Path) -> str:
    base = (ROOT / "version.txt").read_text().split()[0]
    base, sep, suffix = base.partition("-")  # "0.2.11-release" -> suffix "release"
    n = "0"
    cache = build_dir / "CMakeCache.txt"
    if cache.exists():
        m = re.search(r"^APEX_BUILD_NUMBER:\w+=(\d+)", cache.read_text(), re.M)
        if m:
            n = m.group(1)
    parts = base.split(".")
    while len(parts) < 3:
        parts.append("0")
    return ".".join(parts[:3] + [n]) + (f"-{suffix}" if sep else "")


def qt_prefix_from_cache(build_dir: Path):
    cache = build_dir / "CMakeCache.txt"
    if cache.exists():
        m = re.search(r"^Qt6_DIR:\w+=(.+)$", cache.read_text(), re.M)
        if m:
            return Path(m.group(1)).resolve().parents[2]  # <prefix>/lib/cmake/Qt6 -> <prefix>
    return None


def bundle_qt_linux(stage: Path, exe: Path, qt_prefix):
    lib = stage / "lib"
    lib.mkdir(exist_ok=True)
    seen = set()

    def deps(path):
        out = subprocess.run(["ldd", str(path)], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            m = re.match(r"\s*(\S+) => (/\S+)", line)
            if m:
                yield m.group(1), Path(m.group(2))

    todo = [exe]
    plugdir = None
    for _, p in deps(exe):
        if p.name.startswith("libQt6Core"):
            plugdir = p.resolve().parent / "qt6" / "plugins"
            if not plugdir.exists() and qt_prefix:
                plugdir = qt_prefix / "plugins"
    plugins = stage / "plugins"
    if plugdir and plugdir.exists():
        for sub in ("platforms", "iconengines", "imageformats", "wayland-decoration-client",
                    "wayland-graphics-integration-client", "wayland-shell-integration",
                    "xcbglintegrations"):
            if (plugdir / sub).is_dir():
                shutil.copytree(plugdir / sub, plugins / sub, symlinks=False, dirs_exist_ok=True)
        todo += list(plugins.rglob("*.so"))
    else:
        print("warning: Qt plugin directory not found; platform plugins not bundled")
    while todo:
        f = todo.pop()
        for name, path in deps(f):
            if name in seen or SYSTEM_LIB.match(name):
                continue
            seen.add(name)
            shutil.copy2(path.resolve(), lib / name)
            todo.append(path)


def package_linux(build_dir, out, bundle, qt_prefix, ver):
    top = f"apex-{ver}"
    work = out / "stage"
    shutil.rmtree(work, ignore_errors=True)
    stage = work / top
    run(["cmake", "--install", str(build_dir), "--prefix", str(stage)])
    exe = stage / "bin" / "apex"
    if bundle:
        bundle_qt_linux(stage, exe, qt_prefix)
        (stage / "apex").write_text(LAUNCHER)
        (stage / "apex").chmod(0o755)
    archive = out / f"{top}-linux-{platform.machine()}.tar.gz"
    with tarfile.open(archive, "w:gz") as tf:
        tf.add(stage, arcname=top)
    shutil.rmtree(work)
    return archive


def package_windows(build_dir, out, bundle, qt_prefix, ver):
    top = f"apex-{ver}"
    work = out / "stage"
    shutil.rmtree(work, ignore_errors=True)
    stage = work / top
    run(["cmake", "--install", str(build_dir), "--prefix", str(stage)])
    if bundle:
        tool = (qt_prefix / "bin" / "windeployqt.exe") if qt_prefix else None
        exe = str(tool) if tool and tool.exists() else shutil.which("windeployqt")
        if not exe:
            sys.exit("windeployqt not found; pass --qt-prefix or --no-bundle-qt")
        # Include Qt6Svg.dll and both SVG loaders even though icons render through QSvgRenderer.
        run([exe, "--svg", "--include-plugins", "qsvg,qsvgicon", "--no-translations", "--no-system-d3d-compiler", str(stage / "apex.exe")])
    archive = out / f"{top}-windows-x64.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for f in sorted(stage.rglob("*")):
            if f.is_file():
                zf.write(f, f.relative_to(work))
    shutil.rmtree(work)
    return archive


def main():
    win = os.name == "nt"
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--build-dir", type=Path,
                    default=ROOT / "build" / ("windows-msvc-release" if win else "linux-gcc-release"))
    ap.add_argument("--out", type=Path, default=ROOT / "dist")
    ap.add_argument("--qt-prefix", type=Path, default=None)
    ap.add_argument("--no-bundle-qt", action="store_true", help="skip Qt runtime bundling")
    a = ap.parse_args()
    if not (a.build_dir / "CMakeCache.txt").exists():
        sys.exit(f"{a.build_dir} is not a configured build tree (build the `apex` target first)")
    a.out.mkdir(parents=True, exist_ok=True)
    qt = a.qt_prefix or qt_prefix_from_cache(a.build_dir)
    ver = version(a.build_dir)
    fn = package_windows if win else package_linux
    print("wrote", fn(a.build_dir, a.out, not a.no_bundle_qt, qt, ver))


if __name__ == "__main__":
    main()
