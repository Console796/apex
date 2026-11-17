#!/usr/bin/env python3
"""plugin_sdk_out_of_tree: the SDK component is enough to build a plugin, and the result loads (0.9.9, D8).

Steps, all in a temporary directory (nothing is written to the source or build tree):
  1. tools/package.py --sdk writes the SDK archive from the configured build tree (this runs
     `cmake --install --component sdk` inside package.py).
  2. The archive is unpacked and its contents are audited: headers, the CMake package, examples and docs
     only. No file from src/ and nothing outside those directories.
  3. The SDK's examples are configured and built as a separate CMake project with
     find_package(ApexPlugin) and -DCMAKE_PREFIX_PATH=<unpacked SDK>: no Apex include path. The configure
     step exports compile_commands.json, and the test fails if the Apex source tree appears in it.
  4. apex_plugin_sdk_loader loads the built plugins into a headless plugin_host::Host and checks them.

How this stays within the AGENTS.md build rules: those forbid building *Apex* with cmake/ninja directly.
This test builds two tiny single-file plugins (no Qt, no Apex sources) in its own temporary directory, with
the generator, make program and compilers of the Apex build that is running it, one job (-j1), and it runs
under scripts/test.sh's memory cgroup like every other test. Exit 77 means skipped (no usable build tool).
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ALLOWED_PREFIXES = ("include/apex/", "lib/cmake/ApexPlugin/", "share/apex-sdk/examples/", "share/doc/apex-sdk/")
ALLOWED_FILES = {"LICENSE"}


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--package-py", type=Path, required=True)
    ap.add_argument("--build-dir", type=Path, required=True)
    ap.add_argument("--source-dir", type=Path, required=True)
    ap.add_argument("--loader", type=Path, required=True)
    ap.add_argument("--cmake", required=True)
    ap.add_argument("--generator", required=True)
    ap.add_argument("--make-program", default="")
    ap.add_argument("--c-compiler", default="")
    ap.add_argument("--cxx-compiler", default="")
    ap.add_argument("--build-type", default="")
    ap.add_argument("--config", default="", help="configuration for multi-config generators")
    a = ap.parse_args()
    if not shutil.which(a.cmake) and not Path(a.cmake).exists():
        print("cmake not found"); return 77
    linux = platform.system() == "Linux"
    platform_key = "linux-x86_64" if linux else "windows-x86_64"

    with tempfile.TemporaryDirectory(prefix="apex-sdk-test-") as tmp:
        tmp = Path(tmp)
        dist = tmp / "dist"
        env = dict(os.environ)
        env["PATH"] = str(Path(a.cmake).parent) + os.pathsep + env.get("PATH", "") if Path(a.cmake).is_absolute() else env.get("PATH", "")
        run([sys.executable, a.package_py, "--sdk", "--build-dir", a.build_dir, "--out", dist], env=env)
        archives = sorted(dist.glob("apex-plugin-sdk-*.zip"))
        if len(archives) != 1:
            print(f"expected one apex-plugin-sdk-*.zip, found {archives}"); return 1
        sdk_root = tmp / "sdk"
        with zipfile.ZipFile(archives[0]) as z:
            names = z.namelist()
            z.extractall(sdk_root)
        top = {n.split("/")[0] for n in names}
        if len(top) != 1:
            print(f"the archive must have one top-level directory, found {sorted(top)}"); return 1
        prefix = sdk_root / top.pop()
        members = sorted(n.split("/", 1)[1] for n in names if "/" in n and not n.endswith("/"))
        bad = [m for m in members if m not in ALLOWED_FILES and not m.startswith(ALLOWED_PREFIXES)]
        bad += [m for m in members if m.startswith("src/") or "/src/" in m]
        if bad:
            print("the SDK archive holds files outside headers, CMake package, examples and docs:", *bad, sep="\n  "); return 1
        for need in ("include/apex/apex_plugin.h", "include/apex/plugin.hpp", "lib/cmake/ApexPlugin/ApexPluginConfig.cmake",
                     "lib/cmake/ApexPlugin/ApexPluginConfigVersion.cmake", "share/apex-sdk/examples/CMakeLists.txt",
                     "share/apex-sdk/examples/hello-tool/hello.c", "share/apex-sdk/examples/polygon-tool/polygon.cpp"):
            if need not in members:
                print(f"the SDK archive lacks {need}"); return 1
        if not any(m.startswith("share/doc/apex-sdk/") and m.endswith(".md") for m in members):
            print("the SDK archive has no docs"); return 1

        build = tmp / "build"
        configure = [a.cmake, "-S", prefix / "share/apex-sdk/examples", "-B", build, "-G", a.generator,
                     f"-DCMAKE_PREFIX_PATH={prefix}", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                     "-DAPEX_EXAMPLE_PLUGINS=hello_tool;polygon_tool;face_stats"]
        if a.make_program: configure.append(f"-DCMAKE_MAKE_PROGRAM={a.make_program}")
        if a.c_compiler: configure.append(f"-DCMAKE_C_COMPILER={a.c_compiler}")
        if a.cxx_compiler: configure.append(f"-DCMAKE_CXX_COMPILER={a.cxx_compiler}")
        if a.build_type: configure.append(f"-DCMAKE_BUILD_TYPE={a.build_type}")
        run(configure, env=env)
        build_cmd = [a.cmake, "--build", build, "--parallel", "1"]
        if a.config: build_cmd += ["--config", a.config]
        run(build_cmd, env=env)

        # "No Apex include path": the compile database of the nested build must not mention the Apex tree.
        db = build / "compile_commands.json"
        if db.exists():
            text = db.read_text()
            sources = json.loads(text)
            if not sources:
                print("empty compile_commands.json"); return 1
            for token in {str(a.source_dir.resolve()), str(a.source_dir)}:
                if token and token in text.replace("\\\\", "\\"):
                    print(f"the nested build refers to the Apex source tree {token}"); return 1
            if str(prefix) not in text:
                print("the nested build does not use the unpacked SDK's include directory"); return 1
        elif a.generator.startswith("Ninja") or "Makefiles" in a.generator:
            print("compile_commands.json was not written"); return 1

        # D9 layout: <folder>/plugin.kv3 and bin/<platform>/<library>.
        plugins = build / "plugins"
        for folder in ("hello", "polygon", "face-stats"):
            manifest = plugins / folder / "plugin.kv3"
            if not manifest.is_file():
                print(f"missing {manifest}"); return 1
            if f"bin/{platform_key}/" not in manifest.read_text():
                print(f"{manifest} does not name a bin/{platform_key}/ entry"); return 1
            if not list((plugins / folder / "bin" / platform_key).glob("*")):
                print(f"no library in {plugins / folder / 'bin' / platform_key}"); return 1

        run([a.loader, plugins, "--log", "worldspawn", "--perform", "plugin.polygon.tool",
             "--section", "face_properties:plugin.facestats.stats"], env=env)
    print("plugin_sdk_out_of_tree: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        print(f"plugin_sdk_out_of_tree: FAIL: {e}")
        sys.exit(1)
