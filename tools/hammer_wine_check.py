#!/usr/bin/env python3
"""Strata Hammer acceptance check: open VMFs in real Hammer (Wine + Xvfb + lavapipe) and re-save.

    python3 tools/hammer_wine_check.py [--keep] <apex_vmf_dump> <vmf-or-dir>...

For each VMF, a copy is opened in Hammer headlessly. The check passes when Hammer shows the map
(its title bar names the file, no error dialog appears), Ctrl+S then rewrites the copy, and the
re-saved file has the same entity/solid/side ids and planes as the input per `apex_vmf_dump`
(planes compared as oriented plane equations, since Hammer re-derives the three points). Differences are reported, not fatal to the run, but the exit
status is non-zero if any map fails.

Environment: needs wine, Xvfb, libX11/libXtst; the game dir comes from GAME_DIR like
~/.local/bin/strata-hammer (Hammer and its P2:CE install), else APEX_P2CE_DIR. It runs on a private Xvfb display and
hides the Wayland socket, so nothing appears on the real desktop. DXVK is pointed at lavapipe
because DRI3/amdgpu present through Xvfb kills the X connection (observed 2026-10-22).
"""
import argparse
import ctypes
import ctypes.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DISPLAY = ":97"
LVP_ICD = "/usr/share/vulkan/icd.d/lvp_icd.json"
GAME_DIR = os.environ.get("GAME_DIR") or os.environ.get("APEX_P2CE_DIR") or str(
    Path.home() / "snap/steam/common/.local/share/Steam/steamapps/common/Portal 2 Community Edition"
)
ERROR_TITLES = ("error", "warning", "unable", "fatal", "failed", "invalid", "problem", "hammer errors")


class X:
    def __init__(self, display):
        self.x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        self.xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XKeysymToKeycode.restype = ctypes.c_ubyte
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.dpy = self.x11.XOpenDisplay(display.encode())
        if not self.dpy:
            raise RuntimeError(f"cannot open display {display}")

    def focus(self, window_id):
        self.x11.XSetInputFocus(self.dpy, window_id, 1, 0)
        self.x11.XFlush(ctypes.c_void_p(self.dpy))

    def chord(self, *keysyms):
        codes = [self.x11.XKeysymToKeycode(ctypes.c_void_p(self.dpy), k) for k in keysyms]
        for c in codes:
            self.xtst.XTestFakeKeyEvent(ctypes.c_void_p(self.dpy), c, 1, 0)
        for c in reversed(codes):
            self.xtst.XTestFakeKeyEvent(ctypes.c_void_p(self.dpy), c, 0, 0)
        self.x11.XFlush(ctypes.c_void_p(self.dpy))


XK_Control_L, XK_s = 0xFFE3, 0x73


def toplevels(env):
    out = subprocess.run(["xwininfo", "-root", "-children"], env=env, capture_output=True, text=True).stdout
    result = []
    for line in out.splitlines():
        m = re.match(r'\s+(0x[0-9a-f]+) "([^"]*)":', line)
        if m:
            result.append((int(m.group(1), 16), m.group(2)))
    return result


def plane(text):
    """Oriented plane (unit normal, distance): Hammer re-derives and rounds the points on save."""
    n = [float(t) for t in text.replace("(", " ").replace(")", " ").split()]
    a, b, c = n[0:3], n[3:6], n[6:9]
    u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
    cross = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    length = sum(x * x for x in cross) ** 0.5 or 1.0
    unit = tuple(x / length for x in cross)
    return unit + (sum(unit[i] * a[i] for i in range(3)),)


def dump(tool, path):
    out = subprocess.run([tool, str(path)], capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(out.stderr.strip() or "apex_vmf_dump failed")
    ents, solids, sides = [], [], []
    for line in out.stdout.splitlines():
        kind, *rest = line.split("\t")
        if kind == "entity":
            ents.append(int(rest[0]))
        elif kind == "solid":
            solids.append(int(rest[0]))
        else:
            sides.append((int(rest[0]), plane(rest[1])))
    return sorted(ents), sorted(solids), sorted(sides)


def screenshot(env, path):
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "x11grab", "-video_size", "1600x1000",
                    "-i", env["DISPLAY"], "-frames:v", "1", str(path)], env=env, capture_output=True)


def kill_wine(env):
    subprocess.run(["wineserver", "-k"], env=env, capture_output=True)
    time.sleep(1)


def check_one(tool, src, workdir, env, x, timeout):
    work = Path(workdir) / src.name
    shutil.copy(src, work)
    before = dump(tool, work)
    mtime = work.stat().st_mtime_ns
    wine_path = "Z:" + str(work).replace("/", "\\")
    log = open(Path(workdir) / (src.stem + ".hammer.log"), "w")
    proc = subprocess.Popen(
        ["wine", "bin/win64/hammer.exe", "-winecompat", wine_path],
        cwd=GAME_DIR, env=env, stdout=log, stderr=subprocess.STDOUT)
    verdict = "timeout waiting for the map to load"
    main = None
    deadline = time.time() + timeout
    try:
        while time.time() < deadline and proc.poll() is None:
            time.sleep(2)
            windows = toplevels(env)
            errors = [t for _, t in windows if t and any(k in t.lower() for k in ERROR_TITLES)]
            if errors:
                screenshot(env, Path(workdir) / (src.stem + ".error.png"))
                return False, f"error dialog: {errors[0]!r} (screenshot in work dir)"
            for wid, title in windows:
                if title.startswith("Hammer - [") and src.name in title:
                    main = wid
            if main:
                break
        if proc.poll() is not None and not main:
            return False, f"Hammer exited early (code {proc.returncode})"
        if not main:
            return False, verdict
        time.sleep(6)  # let the views settle and any late dialogs appear
        x.focus(main)
        time.sleep(1)
        x.chord(XK_Control_L, XK_s)
        saved = False
        for _ in range(20):
            time.sleep(1)
            errors = [t for _, t in toplevels(env) if t and any(k in t.lower() for k in ERROR_TITLES)]
            if errors:
                return False, f"error dialog after save: {errors[0]!r}"
            if work.stat().st_mtime_ns != mtime:
                saved = True
                break
        if not saved:
            return False, "Ctrl+S did not rewrite the file (a dialog may be open)"
        time.sleep(1)
        after = dump(tool, work)
    finally:
        if proc.poll() is None:
            proc.terminate()
        kill_wine(env)
        log.close()
    problems = []
    for name, b, a in zip(("entities", "solids", "sides"), before, after):
        if name == "sides" and len(b) == len(a) and [i for i, _ in b] == [i for i, _ in a]:
            # Hammer writes planes with ~6 significant digits, so compare with a tolerance.
            bad = [i for (i, p), (_, q) in zip(b, a)
                   if max(abs(x - y) for x, y in zip(p[:3], q[:3])) > 1e-3 or abs(p[3] - q[3]) > 0.5]
            if bad:
                problems.append(f"sides: {len(bad)} planes differ beyond tolerance (first id {bad[0]})")
        elif b != a:
            only_b, only_a = set(b) - set(a), set(a) - set(b)
            problems.append(f"{name}: {len(b)} -> {len(a)} (lost {len(only_b)}, gained {len(only_a)})")
    return (not problems), "; ".join(problems) or "loaded, saved, identical ids and planes"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tool")
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--keep", action="store_true", help="keep the work dir with the re-saved VMFs")
    args = ap.parse_args()

    vmfs = []
    for p in map(Path, args.paths):
        vmfs += sorted(p.rglob("*.vmf")) if p.is_dir() else [p]
    tmp = tempfile.mkdtemp(prefix="apex-hammer-")
    xrd = Path(tmp) / "xrd"
    xrd.mkdir(mode=0o700)
    env = {k: v for k, v in os.environ.items() if k != "WAYLAND_DISPLAY"}
    env.update(DISPLAY=DISPLAY, XDG_RUNTIME_DIR=str(xrd), XDG_SESSION_TYPE="x11", WINEDEBUG="-all",
               VK_DRIVER_FILES=LVP_ICD, WINEPREFIX=os.environ.get("WINEPREFIX", str(Path.home() / ".wine")))
    xvfb = subprocess.Popen(["Xvfb", DISPLAY, "-screen", "0", "1600x1000x24", "-ac"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    failures = 0
    try:
        x = X(DISPLAY)
        for vmf in vmfs:
            try:
                ok, detail = check_one(args.tool, vmf, tmp, env, x, args.timeout)
            except Exception as e:  # noqa: BLE001
                ok, detail = False, f"harness error: {e}"
            failures += not ok
            print(f"[{'ok' if ok else 'FAIL'}] {vmf}: {detail}", flush=True)
    finally:
        kill_wine(env)
        xvfb.terminate()
        if args.keep:
            print(f"work dir kept: {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
