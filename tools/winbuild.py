#!/usr/bin/env python3
"""Rebuild the current working tree on the real Windows CI machine, without the rest of the gate.

    tools/winbuild.py [--no-test] [--clean] [-- ctest args...]

Does what the windows-msvc stage of tools/ci.py does (same tar sync, same environment variables,
same persistent tree, so a later ci.py run stays incremental), but streams the output and writes no
"last green Windows run" marker. Needs APEX_WIN_SSH; APEX_WIN_TARGET must be machine (the VM is
driven by ci.py, which starts and stops it).
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ci  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-test", action="store_true", help="configure and build only")
    ap.add_argument("--clean", action="store_true", help="delete the build directory first")
    ap.add_argument("ctest_args", nargs="*", help="extra ctest arguments, e.g. -R name")
    args = ap.parse_args()

    ssh = os.environ.get("APEX_WIN_SSH")
    if not ssh or not ci.win_machine():
        sys.exit("winbuild: set APEX_WIN_TARGET=machine and APEX_WIN_SSH=user@host (docs/development.md §6)")
    wdir = os.environ.get("APEX_WIN_DIR", "C:/apex-ci")
    wwin = wdir.replace("/", "\\")
    port = os.environ.get("APEX_WIN_SSH_PORT", "22")
    key = os.environ.get("APEX_WIN_SSH_KEY", str(Path.home() / ".ssh" / "id_ed25519_apex_ci"))
    opts = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new", "-i", key,
            "-o", "IdentitiesOnly=yes", "-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=6"]
    sshc = ["ssh", "-p", port] + opts
    scpc = ["scp", "-P", port] + opts
    qt = os.environ.get("APEX_WIN_QT", "C:\\Qt\\6.10.2\\msvc2022_64")
    envcmd = os.environ.get("APEX_WIN_ENV_CMD", ci.DEFAULT_VCVARS)
    root = ci.ROOT

    probe = subprocess.run(sshc + ["-o", "ConnectTimeout=10", ssh, "echo %OS%"], capture_output=True, text=True)
    if probe.returncode != 0:
        sys.exit(f"winbuild: cannot reach {ssh} port {port}: " + (probe.stderr.strip().splitlines() or ["ssh failed"])[-1])
    if probe.stdout.strip() != "Windows_NT":
        sys.exit(f"winbuild: {ssh} does not run commands in cmd.exe (got {probe.stdout.strip()[:40]!r})")

    files = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "-z"], cwd=root,
                           check=True, capture_output=True).stdout
    names = files.decode().split("\0")[:-1]
    listf = ci.state_dir() / "windows_files"
    prev = listf.read_text().split("\n") if listf.exists() else []
    keep = set(names)
    stale = [n for n in prev if n and n not in keep]

    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / "src.tar"
        subprocess.run(["tar", "--null", "-T", "-", "-cf", str(archive)], cwd=root, input=files, check=True)
        subprocess.run(sshc + [ssh, f"if not exist {wwin}\\src mkdir {wwin}\\src"], check=True)
        subprocess.run(scpc + [str(archive), f"{ssh}:{wdir}/"], check=True)

    steps = [f"cd /d {wwin}"]
    if args.clean:
        steps.append("if exist src\\build\\windows-msvc-release rmdir /s /q src\\build\\windows-msvc-release")
    steps += ["tar -xmf src.tar -C src", "cd /d src"]
    steps += [f'(if exist "{n.replace("/", chr(92))}" del /q "{n.replace("/", chr(92))}")' for n in stale]
    steps += [
        envcmd,
        f'set "CMAKE_PREFIX_PATH={qt}"', f'set "CPM_SOURCE_CACHE={wwin}\\cpm-cache"',
        f'set "APEX_VK_DEVICE={os.environ.get("APEX_WIN_VK_DEVICE", "apex-ci-no-gpu")}"',
        "cmake --preset windows-msvc-release -DAPEX_BUILD_NUMBER=0",
        "cmake --build --preset windows-msvc-release",
    ]
    if not args.no_test:
        extra = " ".join(args.ctest_args)
        steps.append(f"ctest --preset windows-msvc-release -j 2 {extra}".rstrip())
    listf.write_text("\n".join(names))  # the machine now matches the tree; later deletions diff against it
    sys.exit(subprocess.run(sshc + [ssh, " && ".join(steps)]).returncode)


if __name__ == "__main__":
    main()
