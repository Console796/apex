#!/usr/bin/env python3
"""Terminal monitor for the local CI gate (tools/ci.py), like top for a CI run.

    tools/ci_top.py [--build N] [--interval SECONDS] [--once]

Reads build/ci/<build>/state.json (written by ci.py on every stage change) and the stage logs.
Shows each stage's state, elapsed time against its historical median, a progress bar (real ninja
[done/total] progress for builds, ctest N/M for tests, elapsed/median otherwise), what a pending
stage is waiting on, an overall ETA from tools/ci_history.py, host load and memory, the Windows VM
and its lock, and recent run times. The selected stage's log tail is shown below the table.

Keys: up/down or j/k select a stage, a/A toggle auto-follow of the running stage, p previous
build, n next build, q quit. --once prints one frame without needing a terminal.
"""
import argparse
import json
import os
import re
import select
import shutil
import sys
import time
from pathlib import Path

import ci_history

ROOT = Path(__file__).resolve().parent.parent
CI_DIR = ROOT / "build" / "ci"

RESET, BOLD, DIM = "\x1b[0m", "\x1b[1m", "\x1b[2m"
COLOR = {"ok": "\x1b[32m", "FAIL": "\x1b[31m", "running": "\x1b[33m", "pending": "\x1b[90m",
         "skipped": "\x1b[90m", "queued": "\x1b[36m"}
ICON = {"ok": "✔", "FAIL": "✘", "running": "▶", "pending": "·", "skipped": "–", "queued": "◌"}
NINJA = re.compile(r"\[(\d+)/(\d+)\]")
CTEST = re.compile(r"Test\s+#(\d+):")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def builds():
    """Build directory names that have a state.json, newest first."""
    if not CI_DIR.is_dir():
        return []
    ds = [d for d in CI_DIR.iterdir() if d.is_dir() and any(d.iterdir())]
    return [d.name for d in sorted(ds, key=activity, reverse=True)]


def activity(d):
    """Latest write in a build directory (state.json, report or any stage log)."""
    return max((p.stat().st_mtime for p in d.iterdir()), default=0)


def ci_pids():
    """pids of running tools/ci.py processes (a run started before state.json existed has none)."""
    pids = []
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            argv = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if len(argv) > 1 and b"python" in argv[0] and argv[1].endswith(b"tools/ci.py"):
            pids.append(int(pid))
    return pids


def load_state(build):
    d = CI_DIR / build
    try:
        return json.loads((d / "state.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pass
    return legacy_state(d)


def legacy_state(d):
    """Best-effort state for a run by an older ci.py: one stage per log, the newest is live."""
    logs = sorted(d.glob("*.log"), key=lambda p: p.stat().st_mtime)
    if not logs:
        return None
    report = d / "report.md"
    finished = report.exists()
    live = not finished and bool(ci_pids())
    first = min(p.stat().st_mtime for p in logs)
    stages = []
    for i, p in enumerate(logs):
        running = live and (i == len(logs) - 1 or time.time() - p.stat().st_mtime < 20)
        stages.append({"name": p.stem, "deps": [], "state": "running" if running else "ok",
                       "start": p.stat().st_ctime if running else first,
                       "end": None if running else p.stat().st_mtime, "seconds": 0.0,
                       "note": "", "log": p.name})
    if finished:
        for line in report.read_text(encoding="utf-8").splitlines():
            cells = [c.strip() for c in line.strip("|").split("|")]
            if line.startswith("|") and len(cells) >= 4 and cells[0] in {s["name"] for s in stages}:
                st = next(s for s in stages if s["name"] == cells[0])
                st.update(state=cells[1], note=cells[3],
                          seconds=float(cells[2].rstrip("s") or 0))
    pids = ci_pids()
    return {"build": d.name, "mode": "legacy, no state.json", "pid": pids[0] if live and pids else 0,
            "started": first, "finished": (report.stat().st_mtime if finished else None),
            "updated": time.time(), "failed": any(s["state"] == "FAIL" for s in stages),
            "stages": stages}


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, TypeError):
        return False


def tail(path, n=40, size=65536):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - size))
            data = f.read().decode("utf-8", errors="replace")
    except OSError:
        return []
    lines = [ANSI.sub("", ln).rstrip() for ln in data.replace("\r", "\n").split("\n")]
    return [ln for ln in lines if ln.strip()][-n:]


def log_progress(stage, lines, expect_tests=None):
    """(fraction or None, detail text) read from the tail of a stage's log."""
    for ln in reversed(lines):
        slices = re.search(r"\[apex_tests\] slice .*\[(\d+)/(\d+) done\]", ln)
        if slices and int(slices.group(2)):
            done, total = int(slices.group(1)), int(slices.group(2))
            return done / total, f"apex_tests slices {done}/{total}"
        m = NINJA.search(ln)
        if m and int(m.group(2)):
            done, total = int(m.group(1)), int(m.group(2))
            return done / total, f"ninja {done}/{total}"
        m = CTEST.search(ln)
        if m:
            n = int(m.group(1))
            if expect_tests:
                return min(1.0, n / expect_tests), f"test #{n}/{expect_tests}"
            return None, f"test #{n}"
    return None, ""


def meminfo():
    info = {}
    try:
        for ln in Path("/proc/meminfo").read_text().splitlines():
            k, v = ln.split(":")
            info[k] = int(v.split()[0]) / 1048576
    except OSError:
        return None
    return info


def vm_line():
    pidfile = Path(os.environ.get("APEX_WINVM_DIR", Path.home() / "winvm")) / "ci.pid"
    try:
        pid = int(pidfile.read_text())
        up = pid_alive(pid)
    except (OSError, ValueError):
        up = False
    sd = ci_history.state_dir()
    lock = ""
    try:
        text = (sd / "winvm.lock").read_text().strip()
        import fcntl
        with open(sd / "winvm.lock") as f:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                lock = f"locked: {text}"
    except OSError:
        pass
    restricted = (sd / "winvm.restrict").exists()
    parts = ["VM " + ("running" if up else "stopped")]
    if lock:
        parts.append(lock)
    if restricted:
        parts.append("RESTRICTED by user")
    return "  ".join(parts)


def bar(frac, width=14):
    frac = 0.0 if frac is None else max(0.0, min(1.0, frac))
    full = int(round(frac * width))
    return "█" * full + "░" * (width - full)


def clip(text, width):
    return text if len(text) <= width else text[:max(0, width - 1)] + "…"


def build_frame(build, sel, width, height, now=None):
    now = now or time.time()
    state = load_state(build) if build else None
    out = []
    if not state:
        return [f"{BOLD}Apex CI{RESET}  no run found under {CI_DIR}", "start one with tools/ci.py"], 0

    stages = state["stages"]
    names = [s["name"] for s in stages]
    alive = pid_alive(state.get("pid")) and not state.get("finished")
    verdict = ("RUNNING" if alive else
               ("FAILED" if state.get("failed") else "PASSED") if state.get("finished") else "ABORTED")
    vcolor = COLOR["running"] if verdict == "RUNNING" else (
        COLOR["ok"] if verdict == "PASSED" else COLOR["FAIL"])
    end = state.get("finished") or (now if alive else state.get("updated", now))
    elapsed = end - state["started"]

    est = ci_history.estimates()
    done_state = {s["name"]: s["state"] for s in stages}
    # Per-stage live progress from logs.
    prog, detail, tails = {}, {}, {}
    for s in stages:
        if s["state"] == "running":
            lines = tail(CI_DIR / build / s["log"])
            tails[s["name"]] = lines
            frac, text = log_progress(s["name"], lines)
            if frac is not None:
                prog[s["name"]] = frac
            detail[s["name"]] = text
    finish, ends, unknown = ci_history.predict(stages, est, now, prog)

    out.append(f"{BOLD}Apex CI{RESET} build {build} [{state.get('mode', '')}]  "
               f"{vcolor}{BOLD}{verdict}{RESET}  elapsed {ci_history.fmt_duration(elapsed)}")
    if verdict == "RUNNING" and state.get("mode", "").startswith("legacy"):
        out.append("ETA unavailable: this run predates state.json (no stage graph or timings)")
    elif verdict == "RUNNING" and finish:
        rem = finish - now
        eta = (f"ETA {ci_history.fmt_duration(rem)} (≈{time.strftime('%H:%M:%S', time.localtime(finish))})"
               if est else "ETA unknown (no history yet)")
        if est and unknown:
            eta += f"  [{unknown} stage(s) lack history, estimate is low]"
        out.append(f"{eta}  overall {bar(elapsed / max(1, elapsed + max(0, rem)), 20)} "
                   f"{100 * elapsed / max(1, elapsed + max(0, rem)):.0f}%")
    else:
        runs = ci_history.recent_runs()
        out.append(f"finished in {ci_history.fmt_duration(elapsed)}" +
                   (f"  (typical {ci_history.fmt_duration(sum(r[1] for r in runs[:5]) / len(runs[:5]))})"
                    if runs else ""))

    load = os.getloadavg()
    mem = meminfo()
    host = f"load {load[0]:.1f}/{os.cpu_count()}"
    if mem:
        host += f"  mem free {mem.get('MemAvailable', 0):.1f}/{mem.get('MemTotal', 0):.1f} GiB"
    out.append(f"{DIM}{host}   {vm_line()}{RESET}")
    out.append("")

    nw = max(len(n) for n in names)
    nw = min(nw, max(20, width // 3))
    out.append(f"{BOLD}    {'stage':<{nw}} {'state':<8} {'time':>7} {'median':>7}  progress{RESET}")
    for i, s in enumerate(stages):
        name, st = s["name"], s["state"]
        shown = st
        if st == "pending" and all(done_state.get(d) == "ok" for d in s["deps"]):
            shown = "queued"  # ready but waiting for a build/configure slot
        med = est.get(name, (None, 0))[0]
        if st == "running":
            t = now - s["start"]
        elif st in ("ok", "FAIL", "skipped"):
            t = s.get("seconds", 0)
        else:
            t = None
        if st == "running":
            frac = prog.get(name)
            if frac is None and med:
                frac = min(0.99, t / med)
            extra = detail.get(name) or (f"~{ci_history.fmt_duration(max(0, ends[name] - now))} left")
            if prog.get(name) is not None:
                extra += f"  ~{ci_history.fmt_duration(max(0, ends[name] - now))} left"
            prog_txt = f"{bar(frac)} {extra}"
        elif st == "pending":
            waiting = [d for d in s["deps"] if done_state.get(d) != "ok"]
            prog_txt = (f"waiting for {', '.join(waiting)}" if waiting else "waiting for a free slot")
        else:
            prog_txt = s.get("note", "")
        row = (f"{'>' if i == sel else ' '} {ICON[shown]} {clip(name, nw):<{nw}} {shown:<8} "
               f"{ci_history.fmt_duration(t) if t is not None else '':>7} "
               f"{ci_history.fmt_duration(med) if med else '':>7}  {prog_txt}")
        row = clip(row, width - 1)
        out.append(f"{COLOR[shown]}{BOLD if i == sel else ''}{row}{RESET}")

    runs = ci_history.recent_runs()
    if runs:
        line = "  ".join(f"#{b} {ci_history.fmt_duration(sec)}{'✘' if bad else ''}"
                          for b, sec, bad, _ts in runs)
        out.append("")
        out.append(clip(f"{DIM}recent: {line}{RESET}", width + 8))

    # Log tail of the selected stage.
    room = height - len(out) - 3
    if room >= 3 and 0 <= sel < len(stages):
        s = stages[sel]
        lines = tails.get(s["name"]) or tail(CI_DIR / build / s["log"], room)
        out.append("")
        out.append(f"{BOLD}── {s['name']} ── {s['log']}{RESET}"
                   + (f"  {COLOR['FAIL']}{s['note']}{RESET}" if s["state"] == "FAIL" else ""))
        for ln in lines[-room:]:
            out.append(clip(ln, width - 1))
        if not lines:
            out.append(f"{DIM}(no log output yet){RESET}")
    return out, len(stages)


def follow_running(state):
    for i, s in enumerate(state["stages"] if state else []):
        if s["state"] == "running":
            return i
    return None


def interactive(args):
    import termios
    import tty
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    names = builds()
    idx = names.index(args.build) if args.build in names else 0
    sel, follow = 0, True
    sys.stdout.write("\x1b[?1049h\x1b[?25l")
    try:
        tty.setcbreak(fd)
        while True:
            names = builds() or names
            build = names[min(idx, len(names) - 1)] if names else None
            state = load_state(build) if build else None
            if follow:
                f = follow_running(state)
                if f is not None:
                    sel = f
            cols, rows = shutil.get_terminal_size()
            frame, count = build_frame(build, sel, cols, rows)
            sel = min(sel, max(0, count - 1))
            sys.stdout.write("\x1b[H" + "\n".join(ln + "\x1b[K" for ln in frame[:rows - 1]) + "\x1b[J")
            sys.stdout.flush()
            if select.select([sys.stdin], [], [], args.interval)[0]:
                key = os.read(fd, 8).decode(errors="ignore")
                if key in ("q", "\x03"):
                    return
                if key in ("\x1b[A", "k"):
                    sel, follow = max(0, sel - 1), False
                elif key in ("\x1b[B", "j"):
                    sel, follow = min(count - 1, sel + 1), False
                elif key in ("a", "A"):
                    follow = not follow
                elif key == "p":
                    idx, sel, follow = min(idx + 1, len(names) - 1), 0, True
                elif key == "n":
                    idx, sel, follow = max(idx - 1, 0), 0, True
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        sys.stdout.write("\x1b[?25h\x1b[?1049l")
        sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", help="build number to show (default: newest)")
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--once", action="store_true", help="print one frame and exit")
    a = ap.parse_args()
    if a.once or not sys.stdout.isatty() or not sys.stdin.isatty():
        names = builds()
        build = a.build or (names[0] if names else None)
        state = load_state(build) if build else None
        sel = follow_running(state) or 0
        cols, rows = shutil.get_terminal_size((120, 40))
        frame, _ = build_frame(build, sel, cols, rows)
        print("\n".join(frame))
        return 0
    try:
        interactive(a)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
