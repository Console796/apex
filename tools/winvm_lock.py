#!/usr/bin/env python3
"""Windows CI VM lock and restriction state (shared by tools/ci.py and tools/winvm.sh).

    tools/winvm_lock.py status      read-only; exit 0 = free, 1 = in use, 2 = restricted

Two independent pieces of state live in $XDG_STATE_HOME/apex (default ~/.local/state/apex):
  winvm.lock      flock(2) held by the one CI run using the VM; holder details are written into it.
                  The kernel drops the lock if the holder dies, so there is no stale-lock cleanup.
  winvm.restrict  exists when the user has locked the VM out of CI; its text is the reason.
                  Written by tools/winvm-lock.sh, removed by tools/winvm-unlock.sh (user-only, TTY).
"""
import fcntl
import os
import sys
import time
from contextlib import contextmanager
from pathlib import Path


class VmUnavailable(Exception):
    """Raised with a caller-facing message when the VM may not be used."""


def state_dir():
    d = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state")) / "apex"
    d.mkdir(parents=True, exist_ok=True)
    return d


def restrict_file():
    return state_dir() / "winvm.restrict"


def restriction():
    """The user's restriction text, or None."""
    try:
        return restrict_file().read_text().strip() or "(no reason given)"
    except FileNotFoundError:
        return None


def check_restriction():
    r = restriction()
    if r:
        raise VmUnavailable(
            "the Windows VM is restricted by the user and is not available to CI:\n"
            f"{r}\n(the user lifts this with tools/winvm-unlock.sh)")


def _holder(f):
    f.seek(0)
    return f.read().strip() or "unknown holder"


@contextmanager
def acquire(what="ci.py", wait=0):
    """Hold the VM exclusively. Raises VmUnavailable if restricted or held by another run
    (after waiting up to `wait` seconds for the holder)."""
    check_restriction()
    f = open(state_dir() / "winvm.lock", "a+")
    deadline = time.time() + wait
    try:
        while True:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.time() >= deadline:
                    raise VmUnavailable(
                        f"the Windows VM is in use by another CI run ({_holder(f)}); "
                        "only one run may use it at a time")
                time.sleep(2)
        check_restriction()  # the user may have restricted it while we waited
        f.seek(0)
        f.truncate()
        f.write(f"{what} pid={os.getpid()} since {time.strftime('%Y-%m-%d %H:%M:%S')} cwd={os.getcwd()}\n")
        f.flush()
        try:
            yield
        finally:
            f.seek(0)
            f.truncate()
    finally:
        f.close()  # releases the flock


def main():
    if sys.argv[1:] != ["status"]:
        print(__doc__, file=sys.stderr)
        return 64
    r = restriction()
    with open(state_dir() / "winvm.lock", "a+") as f:
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            busy = None
        except BlockingIOError:
            busy = _holder(f)
    print("restricted by user:\n" + r if r else "not restricted")
    print(f"in use by: {busy}" if busy else "not in use")
    return 2 if r else 1 if busy else 0


if __name__ == "__main__":
    sys.exit(main())
