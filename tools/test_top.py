#!/usr/bin/env python3
"""Terminal monitor for a direct run of a doctest binary (apex_tests, apex_ui_tests, ...), like ci_top.py.

    tools/test_top.py [--dir DIR] [--binary NAME] [--interval SECONDS] [--once]

tests/main.cpp makes every direct run write build/test-progress/<binary>/ (override the root with
$APEX_TEST_PROGRESS_DIR; APEX_TEST_PROGRESS=0 turns it off; ctest runs never write it):
progress.json (state, done/total, asserts, current case), tests.log (one line per finished case) and
failures.log. This shows the most recently active binary: bar, rate, ETA, the running case with its
elapsed time, the slowest finished cases and the tail of the failures. q quits; --once prints one frame.
"""
import argparse
import re
import json
import os
import select
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESET, BOLD, DIM = "\x1b[0m", "\x1b[1m", "\x1b[2m"
GREEN, RED, YELLOW = "\x1b[32m", "\x1b[31m", "\x1b[33m"


def root_dir(arg):
    return Path(arg or os.environ.get("APEX_TEST_PROGRESS_DIR") or ROOT / "build" / "test-progress")


def runs(root):
    """Binary directories that have a progress.json, most recently written first."""
    ds = [d for d in root.iterdir() if (d / "progress.json").exists()] if root.is_dir() else []
    return sorted(ds, key=lambda d: (d / "progress.json").stat().st_mtime, reverse=True)


def load(d):
    try:
        return json.loads((d / "progress.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def tail(path, n):
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()[-n:]
    except OSError:
        return []


ANSI = re.compile(r"\x1b\[[0-9;]*m")


def clip(line, width):
    """Cut a line to `width` visible columns, keeping its escape codes, so nothing wraps and scrolls the top away."""
    out, seen, pos = [], 0, 0
    for m in ANSI.finditer(line):
        take = max(0, min(m.start() - pos, width - seen))
        out.append(line[pos:pos + take]); seen += take
        out.append(m.group()); pos = m.end()
    out.append(line[pos:pos + max(0, width - seen)])
    return "".join(out) + RESET


def fmt(sec):
    sec = int(sec)
    return f"{sec // 3600}:{sec // 60 % 60:02}:{sec % 60:02}" if sec >= 3600 else f"{sec // 60}:{sec % 60:02}"


def bar(frac, width):
    n = int(max(0.0, min(1.0, frac)) * width)
    return "█" * n + "░" * (width - n)


def frame(d, width, height):
    p = load(d)
    if not p:
        return [f"{d.name}: unreadable progress.json"]
    state = p["state"]
    if state == "running" and not alive(p["pid"]):
        state = "aborted"  # the process is gone without test_run_end (crash or kill)
    color = {"running": YELLOW, "passed": GREEN, "failed": RED, "aborted": RED}[state]
    done, total, failed = p["done"], p["total"], p["failed"]
    frac = done / total if total else 0.0
    # Elapsed is frozen once the run is over; while running it is as of the last write, so extend it.
    elapsed = p["elapsed"] + (max(0.0, time.time() - (d / "progress.json").stat().st_mtime) if state == "running" else 0)
    eta = f"  ETA {fmt(elapsed / done * (total - done))}" if state == "running" and done and total > done else ""
    out = [f"{BOLD}{p['binary']}{RESET}  {color}{state}{RESET}  pid {p['pid']}  elapsed {fmt(elapsed)}{eta}",
           f"{bar(frac, 30)} {done}/{total} cases ({frac * 100:.0f}%)  "
           f"{RED if failed else GREEN}{failed} failed{RESET}  {p['skipped']} skipped  "
           f"{p['asserts']} asserts, {p['assertsFailed']} failed"]
    if p["current"]:
        run = max(0.0, p["currentElapsed"] + (time.time() - (d / "progress.json").stat().st_mtime if state == "running" else 0))
        out.append(f"{YELLOW}▶{RESET} {p['current']}  {DIM}{fmt(run)}  {p['currentFile']}:{p['currentLine']}{RESET}")
    log = tail(d / "tests.log", 100000)
    timed = []
    for line in log:
        parts = line.split(" ", 2)
        try:
            timed.append((float(parts[1].rstrip("s")), parts[0], parts[2]))
        except (IndexError, ValueError):
            pass
    if timed:
        out += ["", f"{BOLD}Slowest finished{RESET}"]
        out += [f"  {t:7.2f}s {RED if s == 'FAIL' else ''}{n}{RESET}" for t, s, n in sorted(timed, reverse=True)[:5]]
    bad = [l for l in log if l.startswith("FAIL")]
    fails = tail(d / "failures.log", 12)
    if bad or fails:
        out += ["", f"{BOLD}{RED}Failures{RESET}"]
        out += [f"  {l}" for l in (fails or bad[-8:])]
    out += ["", f"{BOLD}Recent{RESET}"]
    room = max(0, height - len(out) - 1)
    out += [("  " + (RED if l.startswith("FAIL") else DIM) + l + RESET) for l in log[-room:]] if room else []
    out = [clip(l, width - 1) for l in out]
    return out[:height - 1]  # the stats at the top always win over the log


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", help="progress root (default build/test-progress or $APEX_TEST_PROGRESS_DIR)")
    ap.add_argument("--binary", help="binary name to follow (default: most recently active)")
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--once", action="store_true", help="print one frame and exit")
    a = ap.parse_args()
    root = root_dir(a.dir)

    def pick():
        rs = runs(root)
        if a.binary:
            rs = [d for d in rs if d.name == a.binary]
        return rs[0] if rs else None

    def render():
        size = shutil.get_terminal_size((100, 40))
        d = pick()
        return frame(d, size.columns, size.lines) if d else [f"no test progress under {root} yet (run a test binary directly)"]

    if a.once or not sys.stdout.isatty():
        print("\n".join(render()))
        return
    import termios
    import tty
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)
        sys.stdout.write("\x1b[?1049h\x1b[?25l")
        while True:
            sys.stdout.write("\x1b[H\x1b[J" + "\n".join(render()))
            sys.stdout.flush()
            if select.select([sys.stdin], [], [], a.interval)[0] and sys.stdin.read(1) in "qQ":
                break
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        sys.stdout.write("\x1b[?25h\x1b[?1049l")


if __name__ == "__main__":
    main()
