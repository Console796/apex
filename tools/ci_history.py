"""Stage timing history and ETA prediction shared by tools/ci.py and tools/ci_top.py.

ci.py appends one JSON line per finished stage to $XDG_STATE_HOME/apex/ci_history.jsonl:
    {"build": 12, "stage": "linux-gcc-release:build", "status": "ok", "seconds": 91.3, "ts": ...}
plus a "__total__" line per run (wall clock of the whole gate). Only "ok" samples feed estimates, and
the estimate for a stage is the median of its last HISTORY_WINDOW samples. Because stages run in
parallel, samples already include the contention of a normal run.
"""
import json
import os
import statistics
import time
from pathlib import Path

HISTORY_WINDOW = 5
TOTAL = "__total__"


def state_dir():
    d = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state")) / "apex"
    d.mkdir(parents=True, exist_ok=True)
    return d


def history_file():
    return state_dir() / "ci_history.jsonl"


def record(build, stage, status, seconds):
    line = json.dumps({"build": build, "stage": stage, "status": status,
                       "seconds": round(seconds, 2), "ts": int(time.time())})
    with open(history_file(), "a", encoding="utf-8") as f:
        f.write(line + "\n")


def load():
    """All history rows, oldest first. Unreadable lines are ignored."""
    rows = []
    try:
        with open(history_file(), encoding="utf-8") as f:
            for line in f:
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
    except OSError:
        pass
    return rows


def estimates(rows=None):
    """{stage: (median seconds, sample count)} from the last HISTORY_WINDOW ok samples."""
    samples = {}
    for r in rows if rows is not None else load():
        if r.get("status") == "ok" and r.get("seconds", 0) > 0:
            samples.setdefault(r["stage"], []).append(r["seconds"])
    return {s: (statistics.median(v[-HISTORY_WINDOW:]), len(v[-HISTORY_WINDOW:]))
            for s, v in samples.items()}


def recent_runs(rows=None, count=6):
    """[(build, total seconds, failed?)] newest first, from the __total__ rows."""
    out = [(r["build"], r["seconds"], r["status"] != "ok", r.get("ts", 0))
           for r in (rows if rows is not None else load()) if r.get("stage") == TOTAL]
    return out[::-1][:count]


def predict(stages, est, now=None, progress=None):
    """Predict when each stage and the whole run finish.

    stages: [{"name", "deps", "state", "start", "end"}]; state is pending/running/ok/FAIL/skipped.
    est: estimates(). progress: optional {stage: fraction done in (0,1)} for running stages, which
    refines the estimate (for example ninja's [done/total]).
    Returns (finish timestamp or None, {stage: predicted end timestamp}, number of stages with no
    history). Resource limits (build slots, the VM) are not modelled, so this is a lower bound
    when a run is contended in an unusual way.
    """
    now = now or time.time()
    progress = progress or {}
    ends = {}
    unknown = 0
    by_name = {s["name"]: s for s in stages}

    def end_of(name, seen=()):
        if name in ends:
            return ends[name]
        s = by_name[name]
        if s["state"] in ("ok", "FAIL", "skipped"):
            ends[name] = s.get("end") or now
            return ends[name]
        expected = est.get(name, (None, 0))[0]
        if s["state"] == "running":
            start = s.get("start") or now
            elapsed = now - start
            frac = progress.get(name)
            if frac and frac > 0.02:
                total = elapsed / frac
            elif expected:
                total = max(expected, elapsed)
            else:
                total = elapsed
            ends[name] = start + max(total, elapsed)
        else:
            dep_end = max([end_of(d) for d in s["deps"] if d in by_name] or [now])
            ends[name] = max(dep_end, now) + (expected or 0)
        return ends[name]

    for s in stages:
        if s["state"] in ("pending", "running") and s["name"] not in est:
            unknown += 1
        end_of(s["name"])
    finish = max(ends.values()) if ends else None
    return finish, ends, unknown


def fmt_duration(seconds):
    seconds = max(0, int(round(seconds)))
    m, s = divmod(seconds, 60)
    h, m = divmod(m, 60)
    if h:
        return f"{h}h{m:02d}m"
    return f"{m}m{s:02d}s" if m else f"{s}s"
