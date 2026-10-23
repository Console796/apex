#!/usr/bin/env python3
"""Apex local CI gate (plan 0.1, D22/D23).

    tools/ci.py [--fast] [--windows {auto,always,never}] [--fixtures-only]
                [--fuzz-seconds N]

Stages: preflight, BUILD counter, Linux presets (build + unit tests), KV1 corpus and fuzz (plus a fixed-seed hull fuzz),
srctools crosscheck, GPU render tests on lavapipe (required) and RADV (skipped without a device),
Windows VM, licence + layer checks. Writes
build/ci/<build>/report.md and exits non-zero on any failure. Fuzzing defaults to 600 seconds;
use --fuzz-seconds 20 while iterating, or 0 to disable it explicitly.

The Windows VM stage is the slow one, so --windows defaults to "auto": it runs only when build
files or platform-sensitive code (WIN_TRIGGERS) changed since the last passing Windows run.
Use "always" before tagging a sub-milestone or a release (the gate, D22/D23), "never" to skip.
The guest keeps its build tree between runs, so a run after a small change is incremental.

Environment:
  APEX_P2CE_DIR      P2CE install; required unless --fixtures-only (D20)
  APEX_WIN_SSH       ssh target of the Windows VM, e.g. user@127.0.0.1 (D23)
  APEX_WIN_SSH_PORT  ssh port (default 2222, forwarded by tools/winvm.sh)
  APEX_WIN_SSH_KEY   private key (default ~/.ssh/id_ed25519_apex_ci)
  APEX_WIN_QT        Qt prefix in the guest (default C:\\Qt\\6.10.2\\msvc2022_64)
  APEX_WIN_DIR       work directory in the VM (default C:/apex-ci)
  APEX_WIN_LOCK_WAIT seconds to wait for another run to release the VM (default 0: refuse at once)
  APEX_WIN_MANAGE    1 (default): tools/winvm.sh starts the VM before and shuts it down after;
                     0 leaves it running (for a working session)
  APEX_WIN_ENV_CMD   cmd command that puts MSVC on PATH (default: vcvars64 of VS 2022 Build Tools)
  XDG_STATE_HOME     holds apex/build_number (default ~/.local/state)
"""
import argparse
import datetime
import os
import re
import shlex
import shutil
import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import ci_history
import winvm_lock

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
    """Thread-safe stage table. Stages are planned up front (name + dependencies), then move
    pending -> running -> ok/FAIL/skipped; state.json mirrors that for the terminal monitor."""

    def __init__(self, build, mode=""):
        self.dir = ROOT / "build" / "ci" / str(build)
        self.dir.mkdir(parents=True, exist_ok=True)
        for stale in ("state.json", "report.md"):  # build "fast" reuses its directory
            (self.dir / stale).unlink(missing_ok=True)
        self.lock = threading.RLock()
        self.build = build
        self.mode = mode
        self.started = time.time()
        self.finished = None
        self.stages = {}  # name -> dict, in plan order

    def plan(self, name, deps=()):
        with self.lock:
            self.stages[name] = {"name": name, "deps": list(deps), "state": "pending", "start": None,
                                 "end": None, "seconds": 0.0, "note": "", "log": f"{name}.log"}
            self._save()

    def begin(self, name):
        with self.lock:
            st = self.stages.setdefault(name, {"name": name, "deps": [], "state": "pending",
                                               "start": None, "end": None, "seconds": 0.0,
                                               "note": "", "log": f"{name}.log"})
            st.update(state="running", start=time.time())
            self._save()

    def add(self, stage, status, seconds=0.0, note=""):
        with self.lock:
            st = self.stages.setdefault(stage, {"name": stage, "deps": [], "state": "pending",
                                                "start": None, "end": None, "seconds": 0.0,
                                                "note": "", "log": f"{stage}.log"})
            st.update(state=status, end=time.time(), seconds=seconds, note=note)
            if st["start"] is None:
                st["start"] = st["end"] - seconds
            print(f"[{status:7}] {stage} ({seconds:.1f}s) {note}", flush=True)
            if status in ("ok", "FAIL"):
                ci_history.record(self.build, stage, status, seconds)
            self._save()

    def status(self, name):
        with self.lock:
            return self.stages[name]["state"]

    def finished_stage(self, name):
        return self.status(name) in ("ok", "FAIL", "skipped")

    @property
    def rows(self):
        with self.lock:
            return [(s["name"], s["state"], s["seconds"], s["note"])
                    for s in self.stages.values() if s["state"] in ("ok", "FAIL", "skipped")]

    @property
    def failed(self):
        return any(s == "FAIL" for _, s, _, _ in self.rows)

    def _save(self):
        doc = {"build": self.build, "mode": self.mode, "pid": os.getpid(), "started": self.started,
               "finished": self.finished, "updated": time.time(),
               "failed": any(s["state"] == "FAIL" for s in self.stages.values()),
               "stages": list(self.stages.values())}
        tmp = self.dir / "state.json.tmp"
        tmp.write_text(json.dumps(doc), encoding="utf-8")
        os.replace(tmp, self.dir / "state.json")

    def finish(self):
        with self.lock:
            self.finished = time.time()
            ci_history.record(self.build, ci_history.TOTAL, "FAIL" if self.failed else "ok",
                              self.finished - self.started)
            self._save()
            self.write()

    def write(self):
        lines = [f"# CI build {self.build}", "",
                 f"{datetime.datetime.now().isoformat(timespec='seconds')} - "
                 f"{'FAILED' if self.failed else 'passed'}", "",
                 f"wall clock {ci_history.fmt_duration(time.time() - self.started)}", "",
                 "| Stage | Status | Time | Note |", "|---|---|---|---|"]
        lines += [f"| {a} | {b} | {c:.1f}s | {d} |" for a, b, c, d in self.rows]
        (self.dir / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"report: {self.dir / 'report.md'}")


def run(cmd, report, name, log=None, classify=None, **kw):
    """Run a command, tee output to a log file, record the stage.

    classify(ok, log) -> (status, note) overrides the default ok/FAIL status when a stage needs to
    read its own output (for example to report "skipped" for a missing device).
    """
    t = time.time()
    log = log or report.dir / f"{name}.log"
    with open(log, "w", encoding="utf-8") as f:
        f.write("$ " + " ".join(map(str, cmd)) + "\n")
        f.flush()
        p = subprocess.run(cmd, cwd=kw.pop("cwd", ROOT), stdout=f, stderr=subprocess.STDOUT, **kw)
    ok = p.returncode == 0
    if classify:
        status, note = classify(ok, log)
    else:
        status, note = ("ok" if ok else "FAIL"), ("" if ok else f"see {log}")
    report.add(name, status, time.time() - t, note)
    return status != "FAIL"


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


def configure_stage(preset, build, report):
    return run(["cmake", "--preset", preset, f"-DAPEX_BUILD_NUMBER={build}"], report, f"{preset}:configure")


def build_stage(preset, jobs, report):
    return run(["cmake", "--build", "--preset", preset, "-j", str(jobs)], report, f"{preset}:build")


def test_stage(preset, jobs, report):
    return run(["ctest", "--preset", preset, "-j", str(jobs)], report, f"{preset}:test")


def corpus_stage(corpus, fixtures_only, report):
    t = time.time()
    if not corpus or not Path(corpus).is_dir():
        reason = ("APEX_P2CE_DIR is unset" if not corpus else
                  f"APEX_P2CE_DIR is not a directory: {corpus}")
        report.add("corpus", "skipped" if fixtures_only else "FAIL", time.time() - t, reason)
        return

    log = report.dir / "corpus.log"
    executable = ROOT / "build" / "linux-gcc-release" / "tests" / "apex_tests"
    env = os.environ.copy()
    env["APEX_P2CE_DIR"] = corpus
    cmd = [str(executable), "-tc=*corpus*"]
    try:
        with open(log, "w", encoding="utf-8") as f:
            f.write("$ " + shlex.join(cmd) + "\n")
            f.flush()
            result = subprocess.run(cmd, cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT)
        output = log.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        report.add("corpus", "FAIL", time.time() - t, f"could not run corpus test: {error}")
        return

    counts = re.search(
        r"^KV1 corpus: scanned=(\d+), parsed Strata=(\d+), parsed Legacy=(\d+), "
        r"skipped=(\d+), expected failures=(\d+)$", output, re.MULTILINE)
    if result.returncode != 0:
        report.add("corpus", "FAIL", time.time() - t, f"corpus test failed; see {log}")
    elif not counts:
        report.add("corpus", "FAIL", time.time() - t,
                   f"test did not print KV1 corpus counts (possible doctest skip); see {log}")
    else:
        labels = ["scanned", "parsed Strata", "parsed Legacy", "skipped", "expected failures"]
        note = ", ".join(f"{label}={value}" for label, value in zip(labels, counts.groups()))
        report.add("corpus", "ok", time.time() - t, note)


def fuzzer_unavailable_reason(report):
    compiler = shutil.which("clang++")
    if not compiler:
        return "clang++ is not available"
    log = report.dir / "fuzz-probe.log"
    with tempfile.TemporaryDirectory(prefix="fuzz-probe-", dir=ROOT / "build") as directory:
        executable = Path(directory) / "probe"
        cmd = [compiler, "-std=c++23", "-fsanitize=fuzzer,address,undefined", "-x", "c++", "-",
               "-o", str(executable)]
        source = ('#include <cstddef>\n#include <cstdint>\n'
                  'extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { '
                  'return 0; }\n')
        try:
            result = subprocess.run(cmd, cwd=ROOT, input=source, text=True, capture_output=True)
        except OSError as error:
            log.write_text(str(error) + "\n", encoding="utf-8")
            return f"could not run clang++: {error}"
        log.write_text("$ " + shlex.join(cmd) + "\n" + result.stdout + result.stderr,
                       encoding="utf-8")
    if result.returncode != 0:
        return f"clang++ cannot link libFuzzer with ASan/UBSan; see {log}"
    return ""


def fuzz_configure_stage(build, report):
    t = time.time()
    reason = fuzzer_unavailable_reason(report)
    if reason:
        report.add("fuzz-configure", "skipped", time.time() - t, reason)
        return
    run(["cmake", "--preset", "linux-fuzz", f"-DAPEX_BUILD_NUMBER={build}"], report, "fuzz-configure")


def fuzz_build_stage(jobs, report):
    return run(["cmake", "--build", "--preset", "linux-fuzz", "--target", "kv1_fuzz", "-j", str(jobs)],
               report, "fuzz-build")


def fuzz_run_stage(seconds, report):
    t = time.time()
    log = report.dir / "fuzz.log"
    persistent = ROOT / "build" / "fuzz-corpus" / "kv1"
    persistent.mkdir(parents=True, exist_ok=True)
    artifact_dir = Path(tempfile.mkdtemp(prefix="fuzz-artifacts-", dir=report.dir))
    try:
        with open(log, "w", encoding="utf-8") as f, \
                tempfile.TemporaryDirectory(prefix="kv1-seeds-", dir=ROOT / "build") as seeds:
            seed_dir = Path(seeds)
            fixtures = ROOT / "tests" / "fixtures" / "kv1"
            for source in fixtures.rglob("*"):
                if source.is_file():
                    destination = seed_dir / source.relative_to(fixtures)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, destination)
            fuzz = [str(ROOT / "build" / "linux-fuzz" / "kv1_fuzz"), str(persistent),
                    str(seed_dir), f"-max_total_time={seconds}",
                    f"-artifact_prefix={artifact_dir}{os.sep}"]
            f.write("$ " + shlex.join(fuzz) + "\n")
            f.flush()
            result = subprocess.run(fuzz, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
    except OSError as error:
        report.add("fuzz", "FAIL", time.time() - t, f"could not run fuzz stage: {error}")
        return

    artifacts = sorted(path for path in artifact_dir.rglob("*") if path.is_file())
    if artifacts:
        paths = ", ".join(str(path) for path in artifacts)
        report.add("fuzz", "FAIL", time.time() - t, f"artifact: {paths}; see {log}")
    elif result.returncode != 0:
        report.add("fuzz", "FAIL", time.time() - t, f"libFuzzer exited {result.returncode}; see {log}")
    else:
        report.add("fuzz", "ok", time.time() - t, f"{seconds}s, warm corpus: {persistent}")


def gpu_stage(name, device_filter, report):
    """Render tests (tests/render/) on one Vulkan device picked by APEX_VK_DEVICE (0.1.9).

    The render tests skip (MESSAGE only, so doctest still passes) when Context::create finds no
    matching device, and print "no Vulkan device" to the log. Lavapipe is the required software
    device, so a missing one fails the gate; RADV exists only on some hosts, so it is skipped.
    """
    t = time.time()
    executable = ROOT / "build" / "linux-gcc-release" / "tests" / "apex_tests"
    if not executable.exists():
        report.add(name, "FAIL", time.time() - t, f"{executable} is missing; build linux-gcc-release")
        return
    env = os.environ.copy()
    env["APEX_VK_DEVICE"] = device_filter
    env["APEX_VK_VALIDATION"] = "1"
    required = device_filter == "llvmpipe"
    cmd = [str(executable), "-sf=*/tests/render/*"]

    def classify(ok, log):
        text = log.read_text(encoding="utf-8", errors="replace")
        if "no Vulkan device" in text:
            if required:
                return "FAIL", f"no {device_filter} device; see {log}"
            return "skipped", f"no {device_filter} device on this host; GPU render tests skipped"
        if not ok:
            return "FAIL", f"see {log}"
        counts = re.search(r"test cases:\s+(\d+) \|\s+(\d+) passed", text)
        return "ok", (f"{counts.group(2)}/{counts.group(1)} test cases passed" if counts else "")

    run(cmd, report, name, classify=classify, env=env)


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
        lock = winvm_lock.acquire(f"ci.py build {build}", int(os.environ.get("APEX_WIN_LOCK_WAIT", "0")))
        lock.__enter__()
    except winvm_lock.VmUnavailable as e:
        report.add("windows-msvc", "FAIL", 0, str(e))
        return
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
        # The guest tree persists between runs (build/ and the CPM cache survive). Extract with -m
        # (mtime = now): bsdtar on Windows lands preserved mtimes about an hour early, so Ninja
        # treated stale objects as newer than edited sources. Only files deleted on the host need removing.
        names = files.decode().split("\0")[:-1]
        listf = state_dir() / "windows_files"
        prev = listf.read_text().split("\n") if listf.exists() else []
        keep = set(names)
        stale = [n for n in prev if n and n not in keep]
        subprocess.run(sshc + [ssh, f"if not exist {wwin}\\src mkdir {wwin}\\src"], check=True)
        subprocess.run(scpc + [str(archive), f"{ssh}:{wdir}/"], check=True)
        steps = [
            f"cd /d {wwin}",
            "tar -xmf src.tar -C src", "cd /d src",
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
        lock.__exit__(None, None, None)


def crosscheck_stage(corpus, fixtures_only, report):
    """VMF parse vs srctools (0.1.4 case 3). Needs .venv-tools with srctools installed."""
    py = ROOT / ".venv-tools" / "bin" / "python"
    dump = ROOT / "build" / "linux-gcc-release" / "apex_vmf_dump"
    if not (corpus and Path(corpus).is_dir()):
        report.add("srctools-crosscheck", "skipped" if fixtures_only else "FAIL", 0,
                   "APEX_P2CE_DIR is unset or invalid")
    elif not py.exists() or not dump.exists():
        report.add("srctools-crosscheck", "skipped", 0,
                   "needs .venv-tools (python3 -m venv .venv-tools && .venv-tools/bin/pip install srctools)")
    else:
        run([str(py), "-W", "ignore", str(ROOT / "tools" / "srctools_crosscheck.py"), str(dump), corpus],
            report, "srctools-crosscheck")


class Graph:
    """Stages with dependencies, run on threads. A stage starts when every dependency finished
    "ok"; otherwise it is recorded as skipped. `slot` names a semaphore the stage holds while it
    runs (the stage shows as pending until it gets the slot). A stage function reports its own
    result through report.add(); one that returns without doing so is recorded as FAIL."""

    def __init__(self, report, slots):
        self.report = report
        self.slots = {name: threading.Semaphore(n) for name, n in slots.items()}
        self.tasks = []
        self.done = {}

    def add(self, name, fn, deps=(), slot=None):
        self.report.plan(name, deps)
        self.done[name] = threading.Event()
        self.tasks.append((name, fn, list(deps), slot))

    def _work(self, name, fn, deps, slot):
        report = self.report
        try:
            for d in deps:
                self.done[d].wait()
            bad = [d for d in deps if report.status(d) != "ok"]
            if bad:
                why = "failed" if any(report.status(d) == "FAIL" for d in bad) else "was skipped"
                report.add(name, "skipped", 0, f"{bad[0]} {why}")
                return
            sem = self.slots.get(slot)
            if sem:
                sem.acquire()
            try:
                report.begin(name)
                try:
                    fn()
                except Exception as e:  # a crashing stage must not hang its dependents
                    report.add(name, "FAIL", 0, f"stage crashed: {e!r}")
                if not report.finished_stage(name):
                    report.add(name, "FAIL", 0, "stage ended without a result")
            finally:
                if sem:
                    sem.release()
        finally:
            self.done[name].set()

    def run(self, serial=False):
        if serial:  # tasks were added in a dependency-respecting order
            for task in self.tasks:
                self._work(*task)
            return
        threads = [threading.Thread(target=self._work, args=t, name=t[0], daemon=True)
                   for t in self.tasks]
        for t in threads:
            t.start()
        for t in threads:
            t.join()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fast", action="store_true", help="debug build + tests + checks only (pre-push)")
    ap.add_argument("--windows", choices=["auto", "always", "never"], default="auto",
                    help="Windows VM stage: auto (only after platform-sensitive changes), always, never")
    ap.add_argument("--skip-windows", action="store_true", help="same as --windows never")
    ap.add_argument("--fixtures-only", action="store_true", help="do not require the P2CE corpus")
    ap.add_argument("--fuzz-seconds", type=int, default=600,
                    help="libFuzzer run time (default: 600; 0 disables the stage)")
    ap.add_argument("--build-slots", type=int, default=0,
                    help="concurrent compiles (default: 2, or 1 while the Windows VM runs)")
    ap.add_argument("--test-jobs", type=int, default=0,
                    help="concurrent ctest tests (default: half the cores)")
    ap.add_argument("--serial", action="store_true", help="run one stage at a time")
    a = ap.parse_args()
    if a.fuzz_seconds < 0:
        ap.error("--fuzz-seconds must be non-negative")

    corpus = os.environ.get("APEX_P2CE_DIR")
    build = 0 if a.fast else next_build_number()
    rep = Report(build if build else "fast", "fast" if a.fast else "full")

    run_win, win_why = False, "--fast"
    if not a.fast:
        mode = "never" if a.skip_windows else a.windows
        run_win, win_why = (True, "--windows always") if mode == "always" else (False, "--windows never")
        if mode == "auto":
            run_win, win_why = windows_needed()

    cores = os.cpu_count() or 4
    slots = a.build_slots or (1 if run_win else 2)
    if a.serial:
        slots = 1
    jobs = max(2, cores // slots)
    test_jobs = 1 if a.serial else (a.test_jobs or max(2, cores // 2))
    g = Graph(rep, {"build": slots, "configure": 1})

    def add_preset(preset):
        g.add(f"{preset}:configure", lambda: configure_stage(preset, build, rep), slot="configure")
        g.add(f"{preset}:build", lambda: build_stage(preset, jobs, rep), [f"{preset}:configure"], "build")
        g.add(f"{preset}:test", lambda: test_stage(preset, test_jobs, rep), [f"{preset}:build"])

    # Longest chains first so they win a contended slot: the VM, then the gcc-release fan-out.
    if run_win:
        g.add("windows-msvc", lambda: windows_stage(build, rep))
    for preset in (FAST_PRESETS if a.fast else LINUX_PRESETS):
        add_preset(preset)
    if a.fast:
        rep.add("fuzz", "skipped", 0, "--fast")
    else:
        gcc_build = "linux-gcc-release:build"
        g.add("corpus", lambda: corpus_stage(corpus, a.fixtures_only, rep), [gcc_build])
        g.add("srctools-crosscheck", lambda: crosscheck_stage(corpus, a.fixtures_only, rep), [gcc_build])
        g.add("gpu-lavapipe", lambda: gpu_stage("gpu-lavapipe", "llvmpipe", rep), [gcc_build])
        g.add("gpu-radv", lambda: gpu_stage("gpu-radv", "radv", rep), [gcc_build])
        if a.fuzz_seconds == 0:
            rep.add("fuzz", "skipped", 0, "disabled")
        else:
            g.add("fuzz-configure", lambda: fuzz_configure_stage(build, rep), slot="configure")
            g.add("fuzz-build", lambda: fuzz_build_stage(jobs, rep), ["fuzz-configure"], "build")
            g.add("fuzz", lambda: fuzz_run_stage(a.fuzz_seconds, rep), ["fuzz-build"])
        if not run_win:
            rep.add("windows-msvc", "skipped", 0, win_why)
    g.add("check-licenses", lambda: run([sys.executable, str(ROOT / "tools" / "check_licenses.py")],
                                        rep, "check-licenses"))
    g.add("check-layers", lambda: run([sys.executable, str(ROOT / "tools" / "check_layers.py")],
                                      rep, "check-layers"))

    est = ci_history.estimates()
    finish, _ends, unknown = ci_history.predict(list(rep.stages.values()), est)
    if finish and est:
        note = f" ({unknown} stage(s) without history)" if unknown else ""
        print(f"estimated {ci_history.fmt_duration(finish - time.time())}{note}; "
              f"monitor with tools/ci_top.py", flush=True)
    else:
        print("no timing history yet; the ETA appears after a run. Monitor with tools/ci_top.py", flush=True)

    g.run(a.serial)
    rep.finish()
    return 1 if rep.failed else 0


if __name__ == "__main__":
    sys.exit(main())
