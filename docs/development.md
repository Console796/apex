# Development workflow

How to go from a clean clone to a full `tools/ci.py` run with every stage green, and the day-to-day
loop after that. `CONTRIBUTING.md` holds the rules (licence, sign-off, layers, fixtures); this page is
the procedure.

## 1. Host prerequisites (Linux)

Ubuntu 26.04 is the reference host. Install:

```bash
sudo apt install build-essential g++-15 clang-21 libclang-rt-21-dev cmake ninja-build git python3 python3-venv \
  qt6-base-dev qt6-base-private-dev qt6-multimedia-dev libvulkan-dev vulkan-validationlayers mesa-vulkan-drivers glslc \
  dbus-user-session socat qemu-system-x86 ovmf
```

- **Compilers:** GCC 14+ and Clang 18+, both with C++23. CI builds with both.
- **libFuzzer:** `libclang-rt-*-dev` provides it. Without it, the fuzz stages report *skipped* rather
  than green.
- **Qt 6.10:** the system packages work. Apex needs `GuiPrivate`, so the private headers package is
  required.
- **`glslc`:** comes from the `glslc` package or the LunarG Vulkan SDK (`$VULKAN_SDK/bin` is searched).
- **Lavapipe:** `mesa-vulkan-drivers` provides it, and it's the required GPU test device. RADV is
  used when an AMD GPU is present and skipped otherwise.
- **First configure:** the dependencies in `cmake/deps.cmake` (CPM, pinned commits) need network
  access on the first configure. After that they're cached.

## 2. Clone and build

```bash
git clone <remote> apex && cd apex
cmake --preset linux-asan
cmake --build --preset linux-asan -j6
```

Presets are `linux-gcc-{debug,release}`, `linux-clang-{debug,release}`, `linux-asan` (GCC
ASan+UBSan), `linux-fuzz` (Clang libFuzzer) and `windows-msvc-{debug,release}`. Build trees go
under `build/<preset>/`.

**Memory:** on a 15 GB host, cap jobs at `-j6`, especially for `linux-asan`. An unbounded ASan
build runs out of memory, and only one build should run at a time.

## 3. Tests

```bash
ctest --preset linux-asan                       # everything registered
ASAN_OPTIONS=detect_leaks=0 build/linux-asan/tests/apex_tests -tc='*face*'   # one area
```

- **Watch the tests like CI:** `python3 tools/ci.py --tests [PRESET]` builds and runs `ctest` for one
  preset (default `linux-asan`) and the layer check, with no BUILD number and no other stages. It
  writes `build/ci/tests/`, so `tools/ci_top.py` (live stage table, ctest N/M progress, ETA from the
  gate's timings, log tail) and `tools/ci_status.py` follow it exactly as they follow the full gate.

- **Watch a direct run:** running a doctest binary by hand (`apex_tests`, `apex_ui_tests`) writes
  `build/test-progress/<binary>/` (`progress.json`, `tests.log`, `failures.log`; override the root with
  `APEX_TEST_PROGRESS_DIR`, disable with `APEX_TEST_PROGRESS=0`). `python3 tools/test_top.py` follows the
  latest run: bar, ETA, the running case, slowest cases, failures. `ctest` runs and query flags
  (`-ltc`, `-r=`) don't write it.

- **Pick the tests your change touches.** The full ASan suite takes about 15 minutes, so leave it
  to CI.
- **UI tests** (`apex_ui_tests`) run offscreen. Run them by hand isolated from the desktop
  session, so they can't reach KWin or the session bus:

  ```bash
  env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 APEX_SKIP_RECOVERY=1 dbus-run-session -- build/linux-asan/apex_ui_tests -tc='*aterial*'
  ```

  Don't start a bare nested `kwin_wayland`.

  The real-tool compile gate is `python3 tools/chamber_runner_check.py` with
  `APEX_P2CE_DIR` set and both linux-asan test binaries built. The `compile-chamber` stage of
  `tools/ci.py` runs it. It generates D17's
  Apex-authored chamber and runs `compile_chamber` through the real Fast profile
  (vbsp, vvis, vrad and BSP copy), without launching the game. It also checks a cut cordon and a newly created instance compiled into an addon target. VMFs, snapshots and
  copied BSPs stay under `/tmp`; the install is read-only. The driver uses the
  offscreen environment above, falling back to `DBUS_SESSION_BUS_ADDRESS=disabled:`
  if `dbus-run-session` cannot create a bus. The doctest skips unless both
  `APEX_P2CE_DIR` and `APEX_CHAMBER_VMF` are set.
- **Corpus tests:** tests that read the P2CE corpus skip without `APEX_P2CE_DIR` (see §4).
- **Fixtures:** files in `tests/fixtures/` must be Apex-authored. Valve, P2CE and Strata content
  is never committed.

Before committing, run:

```bash
python3 tools/check_layers.py
```

and also `git diff --check`.

## 4. Environment for the full gate

Put these in your shell profile (for example `~/.zshrc`):

```bash
export APEX_P2CE_DIR="$HOME/.steam/debian-installation/steamapps/common/Portal 2 Community Edition/"
export APEX_WIN_SSH=<user>@<guest address>   # the guest account, see §6
export APEX_WIN_SSH_PORT=2222                # 22 with the libvirt backend
export APEX_WINVM_LIBVIRT=win10              # only with the libvirt backend
```

- **`APEX_P2CE_DIR`:** a P2CE install (Steam). CI reads it but never writes under it. Without it,
  run `tools/ci.py --fixtures-only`; the corpus and crosscheck stages are then skipped.
- **srctools:** the crosscheck stage creates `.venv-tools/` and installs srctools into it on first
  use. That needs network access once.
- **Windows variables:** the other `APEX_WIN_*` settings are listed at the top of `tools/ci.py`.

## 5. The CI gate

CI is local by design (plan 0.1, D22); there are no GitHub Actions.

```bash
python3 tools/ci.py                    # full gate; Windows only when relevant files changed
python3 tools/ci.py --windows always   # before tagging a sub-milestone or release
python3 tools/ci.py --fast             # debug build, tests and checks (the pre-push hook)
python3 tools/ci.py --skip-windows --fuzz-seconds 20   # quick full-ish run while iterating
tools/install-hooks.sh                 # opt-in pre-push hook running --fast
```

The full gate runs these stages:
- the build-number counter;
- `linux-gcc-release`, `linux-clang-release` and `linux-asan`, each with configure, build and test;
- the KV1 corpus test;
- the srctools crosscheck;
- the GPU render tests on lavapipe (required) and RADV (optional);
- libFuzzer: KV1 for 600 s, and a fixed-seed hull fuzz;
- the Windows MSVC build and tests in the VM;
- the native compile-chamber gate (baseline, cordon, created instance/addon target);
- the licence and layer checks.

The report is written to `build/ci/<build>/report.md`. Watch a run with `tools/ci_top.py`, or get a
one-shot summary with `tools/ci_status.py`.

**Green on a clean clone means:**
1. §1 is installed.
2. `APEX_P2CE_DIR` is set.
3. The Windows VM is set up (§6).
4. `python3 tools/ci.py --windows always` exits 0 with no FAIL rows.

The first run is slow: CPM fetches, four cold build trees, a cold guest build, and no history yet
for the time estimate.

## 6. The Windows VM (D23)

`tools/ci.py` copies the working tree to a Windows guest over SSH as a tar, then builds and tests
`windows-msvc-release` there. With `APEX_WIN_MANAGE=1` (the default), `tools/winvm.sh` starts the
guest before the stage and shuts it down after. `tools/winvm-lock.sh` and `tools/winvm-unlock.sh`
let you reserve the VM so CI refuses to start it.

### Guest setup (once)

1. Create a Windows 10 or 11 guest (q35, OVMF, 6 GB RAM, 8 vCPUs, about 100 GB disk).
2. Inside the guest, install:
   - VS 2022 Build Tools, with the C++ workload and MSVC 19.44 or later;
   - CMake, Ninja, Python 3.12, Git;
   - Qt 6.10.2 `msvc2022_64` (aqtinstall) at `C:\Qt\6.10.2\msvc2022_64`;
   - the Vulkan SDK (for `glslc`).

   Include Multimedia when installing Qt: `aqt install-qt windows desktop 6.10.2 win64_msvc2022_64 -m qtmultimedia`.
3. Enable the OpenSSH Server, set it to start automatically, and allow port 22 in the firewall.
4. Generate the host key and authorise it in the guest:

   ```bash
   ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519_apex_ci -N ''
   ```

   - For an administrator account, append the public key to
     `C:\ProgramData\ssh\administrators_authorized_keys`.
   - For a standard account, append it to `C:\Users\<user>\.ssh\authorized_keys`.
5. Check it from the host, using the libvirt address and port 22 if that's how you reach it:

   ```bash
   ssh -i ~/.ssh/id_ed25519_apex_ci -p 2222 <user>@127.0.0.1 cl
   ```

   It must log in without a password prompt and print the MSVC banner. If `cl` isn't on PATH over
   SSH, that's expected; CI calls `vcvars64.bat` first (`APEX_WIN_ENV_CMD`).

### How `tools/winvm.sh` finds the guest

`tools/winvm.sh` runs QEMU directly from `$APEX_WINVM_DIR` (default `~/winvm`). That directory
must hold:
- `win10.qcow2`;
- a writable `OVMF_VARS.fd` copied from `/usr/share/OVMF/OVMF_VARS_4M.fd`.

It forwards `127.0.0.1:$APEX_WINVM_SSH_PORT` (default 2222) to the guest's port 22. Keep the
directory on an SSD.

**libvirt backend.** With `APEX_WINVM_LIBVIRT=<domain>` set, `winvm.sh` drives a libvirt domain
on `qemu:///system` instead: `virsh start`, `virsh shutdown` (destroying it after 120 s), and
`virsh domstate`. Your user must be in the `libvirt` group. The guest is reached at its own NAT
address, so reserve one for its MAC address:

```bash
virsh -c qemu:///system net-update default add ip-dhcp-host "<host mac='52:54:00:24:68:c7' name='win10' ip='192.168.122.50'/>" --live --config
```

This workstation's guest is the domain `win10`, reserved at 192.168.122.50, and uses:

```bash
export APEX_WINVM_LIBVIRT=win10
export APEX_WIN_SSH=Test@192.168.122.50   # the guest account name varies per machine
export APEX_WIN_SSH_PORT=22
```

**Clock.** Give the guest a UTC hardware clock and tell Windows so. Otherwise a libvirt
`localtime` clock can boot the guest an hour off until time sync jumps it mid-build. Ninja then sees
every source as newer than `build.ninja` and fails with "manifest still dirty after 100 tries":

```bash
virsh -c qemu:///system edit win10      # <clock offset='utc'>
```

and in the guest, as an administrator:

```bat
reg add HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation /v RealTimeIsUniversal /t REG_QWORD /d 1 /f
```

Then power the guest off fully (a reboot keeps the old clock) and boot it again.

Check that the guest has the toolchain from step 2 before relying on it. Then run
`python3 tools/ci.py --windows always` once to warm the guest's build tree in `C:\apex-ci`.

## 7. Day to day

1. **Branches.** Recent work lands on `master`. The milestone branches `0.3`, `0.4` and `0.5` were
   used while each milestone was open, and they are merged into `master`. If a milestone gets its
   own branch again, work on that branch and check `git branch` first.
2. **Plans:** each milestone has its plan in `docs/plans/`. Record decisions and *As built* notes
   there as the work lands.
3. **Before each commit:** build `linux-asan` with `-j6`, run the targeted tests and the UI tests
   for the area, then `check_layers.py` and `git diff --check`.
4. **Commits:** sign every commit (`git commit -s`), one per sub-milestone (`0.3.9: ...`), with
   the message saying what was built and why.
5. **Before tagging or merging to `master`:** `python3 tools/ci.py --windows always` must be green.
6. **Never write under** `$APEX_P2CE_DIR`, `~/.steam` or a personal map directory. Test maps
   belong in `tests/fixtures/` (Apex-authored) or are generated by tools such as
   `tools/make_probe_vmfs.py`.


### Parallel CI tests

Linux test stages invoke `build/<preset>/tests/apex_tests --jobs=N`. The harness balances small
case slices across worker processes, isolates their temporary directories and merges their progress.
`--test-jobs N` selects N (the automatic default remains CPU- and memory-capped); `--serial` uses one.
CTest then runs the UI and measurement entries with the `apex-tests` label excluded, so core cases
are not repeated. Per-preset progress is under `build/ci/<run>/<preset>/test-progress`, and
`ci_top.py` shows completed slices. Windows retains `ctest -j N`: the current `--jobs` harness is POSIX-only.
The dedicated corpus and GPU oracle stages retain serial binary output for their coverage checks.
