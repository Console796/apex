#!/usr/bin/env bash
# USER-ONLY: restrict the Windows CI VM. While restricted, tools/ci.py (--windows always/auto)
# and `tools/winvm.sh start` refuse to use it and show the reason below to the caller.
#   tools/winvm-lock.sh [reason...]
# Requires an interactive terminal, so agents running without a TTY cannot call it.
set -euo pipefail
if [[ ! -t 0 || ! -t 1 ]]; then
  echo "winvm-lock: must be run by the user from an interactive terminal" >&2
  exit 1
fi
f="${XDG_STATE_HOME:-$HOME/.local/state}/apex/winvm.restrict"
mkdir -p "$(dirname "$f")"
reason="$*"
if [[ -z "$reason" ]]; then read -r -p "Reason shown to CI callers: " reason; fi
{
  echo "Locked by $(id -un) at $(date '+%Y-%m-%d %H:%M:%S')."
  echo "Reason: ${reason:-(none given)}"
} > "$f"
echo "Windows VM restricted:"
cat "$f"
if ! "$(dirname "$0")/winvm_lock.py" status | grep -q "not in use"; then
  echo "note: a CI run is using the VM now; it is not interrupted, new runs are refused."
fi
