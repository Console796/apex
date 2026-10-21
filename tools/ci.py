#!/usr/bin/env python3
"""Apex local CI gate (plan 0.1, D22/D23).

    tools/ci.py [--fast] [--windows {auto,always,never}] [--fixtures-only]

Stages: preflight, BUILD counter, Linux presets (build + unit tests), corpus / srctools / fuzz / GPU
(wired up but empty until their sub-milestones), Windows VM, licence + layer checks. Writes
build/ci/<build>/report.md and exits non-zero on any failure.

The Windows VM stage is the slow one, so --windows defaults to "auto": it runs only when build
files or platform-sensitive code (WIN_TRIGGERS) changed since the last passing Windows run.
Use "always" before tagging a sub-milestone (the release gate, D22/D23), "never" to skip.
The guest keeps its build tree between runs, so a run after a small change is incremental.

Environment:
  APEX_P2CE_DIR      P2CE install; required unless --fixtures-only (D20)
  APEX_WIN_SSH       ssh target of the Windows VM, e.g. user@127.0.0.1 (D23)
  APEX_WIN_SSH_PORT  ssh port (default 2222, forwarded by tools/winvm.sh)
  APEX_WIN_SSH_KEY   private key (default ~/.ssh/id_ed25519_apex_ci)
  APEX_WIN_QT        Qt prefix in the guest (default C:\\Qt\\6.10.2\\msvc2022_64)
  APEX_WIN_DIR       work directory in the VM (default C:/apex-ci)
  APEX_WIN_MANAGE    1 (default): tools/winvm.sh starts the VM before and shuts it down after;
                     0 leaves it running (for a working session)
  APEX_WIN_ENV_CMD   cmd command that puts MSVC on PATH (default: vcvars64 of VS 2022 Build Tools)
  XDG_STATE_HOME     holds apex/build_number (default ~/.local/state)
"""
import argparse
import datetime
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINUX_PRESETS = ["linux-gcc-release", "linux-clang-release", "linux-asan"]
FAST_PRESETS = ["linux-gcc-debug"]
# Paths whose changes can behave differently under MSVC/Windows (build files, filesystem and path
# handling, GPU and Qt code). A change outside these waits for the next trigger or a release run.
WIN_TRIGGERS = ["CMakeLists.txt", "CMakePresets.json", "cmake/", "tests/CMakeLists.txt", "version.txt",
                "src/core/", "src/assets/", "src/render/", "src/ui/", "src/app/"]
DEFAULT_VCVARS = ('call "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools'
                  '\\VC\\Auxiliary\\Build\\vcvars64.bat"')


class Report:
    def __init__(self, build):
        self.dir = ROOT / "build" / "ci" / str(build)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.rows = []
        self.build = build

    def add(self, stage, status, seconds=0.0, note=""):
        self.rows.append((stage, status, seconds, note))
        print(f"[{status:7}] {stage} ({seconds:.1f}s) {note}", flush=True)

    @property
    def failed(self):
        return any(s == "FAIL" for _, s, _, _ in self.rows)

    def write(self):
        lines = [f"# CI build {self.build}", "",
                 f"{datetime.datetime.now().isoformat(timespec='seconds')} - "
                 f"{'FAILED' if self.failed else 'passed'}", "",
                 "| Stage | Status | Time | Note |", "|---|---|---|---|"]
        lines += [f"| {a} | {b} | {c:.1f}s | {d} |" for a, b, c, d in self.rows]
        (self.dir / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"report: {self.dir / 'report.md'}")


def run(cmd, report, name, log=None, **kw):
    """Run a command, tee output to a log file, record the stage."""
    t = time.time()
    log = log or report.dir / f"{name}.log"
    with open(log, "w", encoding="utf-8") as f:
        f.write("$ " + " ".join(map(str, cmd)) + "\n")
        f.flush()
        p = subprocess.run(cmd, cwd=kw.pop("cwd", ROOT), stdout=f, stderr=subprocess.STDOUT, **kw)
    ok = p.returncode == 0
    report.add(name, "ok" if ok else "FAIL", time.time() - t, "" if ok else f"see {log}")
    return ok


def next_build_number():
    f = state_dir() / "build_number"
    n = int(f.read_text().strip() or 0) + 1 if f.exists() else 1
    f.write_text(f"{n}\n")  # incremented once per run, pass or fail (D1)
    return n


def state_dir():
    d = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state")) / "apex"
    d.mkdir(parents=True, exist_ok=True)
    return d


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)


def windows_needed():
    """(run?, reason) for --windows auto."""
    f = state_dir() / "windows_green"
    last = f.read_text().strip() if f.exists() else ""
    if not last or git("cat-file", "-e", last + "^{commit}").returncode != 0:
        return True, "no previous passing Windows run"
    changed = git("diff", "--name-only", last).stdout.split()
    changed += git("ls-files", "--others", "--exclude-standard").stdout.split()
    hit = [c for c in changed if any(c == t or (t.endswith("/") and c.startswith(t)) for t in WIN_TRIGGERS)]
    if hit:
        return True, f"{len(hit)} platform-sensitive file(s) changed since last green run, e.g. {hit[0]}"
    return False, f"no platform-sensitive changes since last green Windows run ({last[:10]})"


def linux_stage(preset, build, report):
    if not run(["cmake", "--preset", preset, f"-DAPEX_BUILD_NUMBER={build}"], report, f"{preset}:configure"):
        return
    if not run(["cmake", "--build", "--preset", preset], report, f"{preset}:build"):
        return
    run(["ctest", "--preset", preset], report, f"{preset}:test")


def windows_stage(build, report):
    ssh = os.environ.get("APEX_WIN_SSH")
    if not ssh:
        report.add("windows-msvc", "FAIL", 0, "APEX_WIN_SSH unset (VM not set up, D23); use --skip-windows")
        return
    wdir = os.environ.get("APEX_WIN_DIR", "C:/apex-ci")
    port = os.environ.get("APEX_WIN_SSH_PORT", "2222")
    manage = os.environ.get("APEX_WIN_MANAGE", "1") == "1"
    key = os.environ.get("APEX_WIN_SSH_KEY", str(Path.home() / ".ssh" / "id_ed25519_apex_ci"))
    opts = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new", "-i", key,
            "-o", "IdentitiesOnly=yes"]
    sshc = ["ssh", "-p", port] + opts
    scpc = ["scp", "-P", port] + opts
    envcmd = os.environ.get("APEX_WIN_ENV_CMD", DEFAULT_VCVARS)
    t = time.time()
    tmp = report.dir
    archive = tmp / "src.tar"
    wwin = wdir.replace("/", "\\")
    qt = os.environ.get("APEX_WIN_QT", "C:\\Qt\\6.10.2\\msvc2022_64")
    try:
        if manage:
            subprocess.run([str(ROOT / "tools" / "winvm.sh"), "start"], check=True)
            for _ in range(60):  # wait up to 5 min for sshd in the guest
                if subprocess.run(sshc + ["-o", "ConnectTimeout=5", ssh, "exit"],
                                  capture_output=True).returncode == 0:
                    break
                time.sleep(5)
        # Working tree as a tar: tracked + untracked files that are not gitignored, so the VM
        # never sees build output or junk but does see uncommitted work.
        files = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z"], cwd=ROOT,
                               check=True, capture_output=True).stdout
        subprocess.run(["tar", "--null", "-T", "-", "-cf", str(archive)], cwd=ROOT, input=files,
                       check=True)
        # The guest tree persists between runs (build/ and the CPM cache survive), so tar's
        # preserved mtimes keep Ninja incremental. Only files deleted on the host need removing.
        names = files.decode().split("\0")[:-1]
        listf = state_dir() / "windows_files"
        prev = listf.read_text().split("\n") if listf.exists() else []
        keep = set(names)
        stale = [n for n in prev if n and n not in keep]
        subprocess.run(sshc + [ssh, f"if not exist {wwin}\\src mkdir {wwin}\\src"], check=True)
        subprocess.run(scpc + [str(archive), f"{ssh}:{wdir}/"], check=True)
        steps = [
            f"cd /d {wwin}",
            "tar -xf src.tar -C src", "cd /d src",
        ] + [f'(if exist "{n.replace("/", chr(92))}" del /q "{n.replace("/", chr(92))}")' for n in stale] + [
            envcmd,
            f'set "CMAKE_PREFIX_PATH={qt}"', f'set "CPM_SOURCE_CACHE={wwin}\\cpm-cache"',
            f"cmake --preset windows-msvc-release -DAPEX_BUILD_NUMBER={build}",
            "cmake --build --preset windows-msvc-release",
            "ctest --preset windows-msvc-release",
        ]
        log = tmp / "windows-msvc.log"
        with open(log, "w", encoding="utf-8") as f:
            p = subprocess.run(sshc + [ssh, " && ".join(steps)], stdout=f, stderr=subprocess.STDOUT)
        ok = p.returncode == 0
        if ok:
            listf.write_text("\n".join(names))
            (state_dir() / "windows_green").write_text(git("rev-parse", "HEAD").stdout.strip() + "\n")
        report.add("windows-msvc", "ok" if ok else "FAIL", time.time() - t, "" if ok else f"see {log}")
    except (subprocess.CalledProcessError, OSError) as e:
        report.add("windows-msvc", "FAIL", time.time() - t, f"VM sync failed: {e}")
    finally:
        if manage:
            subprocess.run([str(ROOT / "tools" / "winvm.sh"), "stop"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fast", action="store_true", help="debug build + tests + checks only (pre-push)")
    ap.add_argument("--windows", choices=["auto", "always", "never"], default="auto",
                    help="Windows VM stage: auto (only after platform-sensitive changes), always, never")
    ap.add_argument("--skip-windows", action="store_true", help="same as --windows never")
    ap.add_argument("--fixtures-only", action="store_true", help="do not require the P2CE corpus")
    a = ap.parse_args()

    corpus = os.environ.get("APEX_P2CE_DIR")
    if not a.fast and not a.fixtures_only and not (corpus and Path(corpus).is_dir()):
        print("APEX_P2CE_DIR must point at a P2CE install (D20); use --fixtures-only to bypass.",
              file=sys.stderr)
        return 2

    build = 0 if a.fast else next_build_number()
    rep = Report(build if build else "fast")

    presets = FAST_PRESETS if a.fast else LINUX_PRESETS
    for p in presets:
        linux_stage(p, build, rep)

    if not a.fast:
        # Wired up, empty until their sub-milestones land.
        for stage, since in [("corpus", "0.1.3"), ("srctools-crosscheck", "0.1.4"),
                             ("fuzz", "0.1.3"), ("gpu-lavapipe", "0.1.9"), ("gpu-radv", "0.1.9")]:
            rep.add(stage, "pending", 0, f"not implemented until {since}")
        mode = "never" if a.skip_windows else a.windows
        run_win, why = (True, "--windows always") if mode == "always" else (False, "--windows never")
        if mode == "auto":
            run_win, why = windows_needed()
        if run_win:
            windows_stage(build, rep)
        else:
            rep.add("windows-msvc", "skipped", 0, why)

    run([sys.executable, str(ROOT / "tools" / "check_licenses.py")], rep, "check-licenses")
    run([sys.executable, str(ROOT / "tools" / "check_layers.py")], rep, "check-layers")

    rep.write()
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
