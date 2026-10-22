#!/usr/bin/env bash
# USER-ONLY: lift the restriction set by tools/winvm-lock.sh so CI may use the Windows VM again.
# Requires an interactive terminal, so agents running without a TTY cannot call it.
set -euo pipefail
if [[ ! -t 0 || ! -t 1 ]]; then
  echo "winvm-unlock: must be run by the user from an interactive terminal" >&2
  exit 1
fi
f="${XDG_STATE_HOME:-$HOME/.local/state}/apex/winvm.restrict"
if [[ ! -f "$f" ]]; then echo "Windows VM was not restricted."; exit 0; fi
cat "$f"
read -r -p "Lift this restriction? [y/N] " a
[[ "$a" == [yY]* ]] || { echo "unchanged"; exit 1; }
rm -f "$f"
echo "Windows VM available to CI."
