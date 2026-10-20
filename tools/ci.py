#!/usr/bin/env python3
"""Apex local CI gate (plan 0.1, D22/D23).

    tools/ci.py [--fast] [--skip-windows] [--fixtures-only]

Stages: preflight, BUILD counter, Linux presets (build + unit tests), corpus / srctools / fuzz / GPU
(wired up but empty until their sub-milestones), Windows VM, licence + layer checks. Writes
build/ci/<build>/report.md and exits non-zero on any failure.

Environment:
  APEX_P2CE_DIR      P2CE install; required unless --fixtures-only (D20)
  APEX_WIN_SSH       ssh target of the Windows VM, e.g. user@127.0.0.1 (D23)
  APEX_WIN_SSH_PORT  ssh port (default 2222, forwarded by tools/winvm.sh)
  APEX_WIN_SSH_KEY   private key (default ~/.ssh/id_ed25519_apex_ci)
  APEX_WIN_QT        Qt prefix in the guest (default C:\\Qt\\6.10.2\\msvc2022_64)
  APEX_WIN_DIR       work directory in the VM (default C:/apex-ci)
  APEX_WIN_MANAGE    1 (default): tools/winvm.sh starts the VM before and stops it after
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
    state = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state")) / "apex"
    state.mkdir(parents=True, exist_ok=True)
    f = state / "build_number"
    n = int(f.read_text().strip() or 0) + 1 if f.exists() else 1
    f.write_text(f"{n}\n")  # incremented once per run, pass or fail (D1)
    return n


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
        subprocess.run(sshc + [ssh, f"if not exist {wwin} mkdir {wwin}"], check=True)
        subprocess.run(scpc + [str(archive), f"{ssh}:{wdir}/"], check=True)
        steps = [
            f"cd /d {wwin}", "(if exist src rmdir /s /q src)", "mkdir src",
            "tar -xf src.tar -C src", "cd /d src",
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
        report.add("windows-msvc", "ok" if ok else "FAIL", time.time() - t, "" if ok else f"see {log}")
    except (subprocess.CalledProcessError, OSError) as e:
        report.add("windows-msvc", "FAIL", time.time() - t, f"VM sync failed: {e}")
    finally:
        if manage:
            subprocess.run([str(ROOT / "tools" / "winvm.sh"), "stop"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fast", action="store_true", help="debug build + tests + checks only (pre-push)")
    ap.add_argument("--skip-windows", action="store_true")
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
        if a.skip_windows:
            rep.add("windows-msvc", "skipped", 0, "--skip-windows")
        else:
            windows_stage(build, rep)

    run([sys.executable, str(ROOT / "tools" / "check_licenses.py")], rep, "check-licenses")
    run([sys.executable, str(ROOT / "tools" / "check_layers.py")], rep, "check-layers")

    rep.write()
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
