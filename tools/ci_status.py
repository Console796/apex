#!/usr/bin/env python3
"""Small Tk window that tracks the local CI gate (tools/ci.py).

    tools/ci_status.py [--interval SECONDS]

Reads build/ci/<build>/. A finished run has report.md (stage table, pass/fail). A run in progress
has only per-stage logs: the most recently written log is the stage being run, earlier ones are
done. Whether ci.py is alive is read from /proc. Refreshes every 2 seconds.
"""
import argparse
import os
import re
import sys
import time
import tkinter as tk
from pathlib import Path
from tkinter import ttk

ROOT = Path(__file__).resolve().parent.parent
CI_DIR = ROOT / "build" / "ci"

BG, PANEL, FG, SEL = "#1e1e1e", "#252526", "#d4d4d4", "#094771"
# Banner fills are darker than the row text colours so white banner text stays readable.
COLORS = {"ok": "#66bb6a", "passed": "#66bb6a", "FAIL": "#ef5350", "FAILED": "#ef5350",
          "skipped": "#9e9e9e", "running": "#ffa726", "done": "#66bb6a", "idle": "#9e9e9e",
          "incomplete": "#9e9e9e"}
BANNER = {"#66bb6a": "#2e7d32", "#ef5350": "#c62828", "#ffa726": "#e65100", "#9e9e9e": "#555555"}


def ci_running():
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            argv = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if len(argv) > 1 and b"python" in argv[0] and argv[1].endswith(b"tools/ci.py"):
            return True
    return False


def build_dirs():
    if not CI_DIR.is_dir():
        return []
    dirs = [d for d in CI_DIR.iterdir() if d.is_dir()]
    return sorted(dirs, key=lambda d: d.stat().st_mtime, reverse=True)


def parse_report(path):
    """Return (verdict, finished-at, [(stage, status, time, note)])."""
    text = path.read_text(encoding="utf-8")
    verdict = "FAILED" if "- FAILED" in text else "passed"
    stamp = re.search(r"^(\S+) - ", text, re.M)
    rows = []
    for line in text.splitlines():
        if line.startswith("|") and not line.startswith("| Stage") and not line.startswith("|---"):
            cells = [c.strip() for c in line.strip("|").split("|")]
            rows.append((cells + [""] * 4)[:4])
    return verdict, stamp.group(1) if stamp else "", rows


def parse_logs(d, live):
    """Stages of a run with no report: log stem = stage name, newest log is the live one."""
    logs = sorted(d.glob("*.log"), key=lambda p: p.stat().st_mtime)
    rows = []
    for i, p in enumerate(logs):
        last = i == len(logs) - 1
        status = "running" if last and live else "done"
        rows.append((p.stem, status, f"{p.stat().st_mtime - logs[0].stat().st_mtime:.0f}s in", ""))
    return rows


def snapshot():
    live = ci_running()
    dirs = build_dirs()
    runs = []
    for d in dirs[:12]:
        rep = d / "report.md"
        if rep.exists():
            verdict, stamp, rows = parse_report(rep)
            runs.append((d.name, verdict, stamp, rows))
        else:
            is_live = live and d is dirs[0]
            runs.append((d.name, "running" if is_live else "incomplete", "",
                         parse_logs(d, is_live)))
    return live, runs


class App(tk.Tk):
    def __init__(self, interval):
        super().__init__()
        self.title("Apex CI status")
        self.geometry("720x460")
        self.interval = int(interval * 1000)
        self.configure(bg=BG)
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("Treeview", background=PANEL, fieldbackground=PANEL, foreground=FG,
                        bordercolor=BG, lightcolor=PANEL, darkcolor=PANEL, rowheight=22)
        style.map("Treeview", background=[("selected", SEL)], foreground=[("selected", "white")])
        style.configure("Treeview.Heading", background="#333333", foreground=FG,
                        bordercolor=BG, relief="flat")
        style.map("Treeview.Heading", background=[("active", "#3c3c3c")])
        style.configure("TPanedwindow", background=BG)
        self.banner = tk.Label(self, font=("sans", 20, "bold"), fg="white", pady=10)
        self.banner.pack(fill="x")
        self.sub = tk.Label(self, anchor="w", padx=8, bg=BG, fg=FG)
        self.sub.pack(fill="x")

        body = ttk.PanedWindow(self, orient="horizontal")
        body.pack(fill="both", expand=True, padx=6, pady=6)
        self.runs = ttk.Treeview(body, columns=("build", "state"), show="headings", height=12)
        self.runs.heading("build", text="Build")
        self.runs.heading("state", text="Result")
        self.runs.column("build", width=70)
        self.runs.column("state", width=90)
        self.runs.bind("<<TreeviewSelect>>", lambda _e: self.show_stages())
        body.add(self.runs, weight=1)
        self.stages = ttk.Treeview(body, columns=("stage", "status", "time", "note"),
                                   show="headings")
        for col, w in (("stage", 190), ("status", 70), ("time", 70), ("note", 200)):
            self.stages.heading(col, text=col.capitalize())
            self.stages.column(col, width=w)
        body.add(self.stages, weight=3)
        for tree in (self.runs, self.stages):
            for tag, color in COLORS.items():
                tree.tag_configure(tag, foreground=color)

        self.data = []
        self.refresh()

    def refresh(self):
        live, self.data = snapshot()
        if live:
            text, color = "RUNNING", COLORS["running"]
        elif not self.data:
            text, color = "NO RUNS", COLORS["idle"]
        else:
            done = next((r for r in self.data if r[1] in ("passed", "FAILED")), None)
            text = (done[1] if done else "NO RESULT").upper()
            color = COLORS.get(done[1] if done else "idle")
        self.banner.config(text=text, bg=BANNER[color])
        latest = self.data[0] if self.data else None
        self.sub.config(text=(f"latest build {latest[0]}: {latest[1]} {latest[2]}"
                              if latest else f"nothing under {CI_DIR}")
                        + f"   (updated {time.strftime('%H:%M:%S')})")

        sel = self.runs.selection()
        keep = self.runs.item(sel[0], "values")[0] if sel else None
        self.runs.delete(*self.runs.get_children())
        for build, state, _stamp, _rows in self.data:
            iid = self.runs.insert("", "end", values=(build, state), tags=(state,))
            if str(build) == keep or (keep is None and build == self.data[0][0]):
                self.runs.selection_set(iid)
        self.show_stages()
        self.after(self.interval, self.refresh)

    def show_stages(self):
        sel = self.runs.selection()
        if not sel:
            return
        build = str(self.runs.item(sel[0], "values")[0])
        self.stages.delete(*self.stages.get_children())
        for run in self.data:
            if run[0] == build:
                for row in run[3]:
                    self.stages.insert("", "end", values=row, tags=(row[1],))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--interval", type=float, default=2.0)
    a = ap.parse_args()
    App(a.interval).mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
