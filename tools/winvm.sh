#!/usr/bin/env bash
# Headless launcher for the Windows CI guest (plan 0.1, D23). Same hardware as
# $APEX_WINVM_DIR/start.sh, but no window, SSH forwarded to 127.0.0.1:$APEX_WINVM_SSH_PORT,
# and a QMP socket for graceful shutdown.
#   tools/winvm.sh start|stop|status
#
# Keep $APEX_WINVM_DIR on an SSD. Suspend-to-file (QMP migrate) was tried and fails on this
# host with "Bad address" at completion regardless of CPU model, so stop is a plain ACPI shutdown.
# Set APEX_WINVM_DISPLAY=gtk to get a window (setup/debugging).
set -euo pipefail
dir="${APEX_WINVM_DIR:-$HOME/winvm}"
port="${APEX_WINVM_SSH_PORT:-2222}"
pidfile="$dir/ci.pid"
qmp="$dir/ci.qmp"

running() { [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; }

# qmp_cmd '<json>': one command on a fresh QMP connection; prints the replies.
qmp_cmd() {
  printf '{"execute":"qmp_capabilities"}\n%s\n' "$1" | timeout 10 socat -t1 - "UNIX-CONNECT:$qmp"
}

wait_exit() { # wait_exit <tries>: poll 2s apart
  for _ in $(seq "$1"); do running || return 0; sleep 2; done
  return 1
}

poweroff() {
  qmp_cmd '{"execute":"system_powerdown"}' >/dev/null || true
  wait_exit 60 && return 0
  echo "guest did not power down in 120s; killing" >&2
  kill "$(cat "$pidfile")"
}

case "${1:-}" in
  start)
    if running; then echo "already running"; exit 0; fi
    cd "$dir"
    qemu-system-x86_64 -name win10-ci -machine q35,accel=kvm \
      -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time \
      -smp "${CPUS:-8}" -m "${RAM:-8G}" \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
      -drive if=pflash,format=raw,file=OVMF_VARS.fd \
      -drive id=disk0,file=win10.qcow2,format=qcow2,if=none,cache=writeback,discard=unmap \
      -device ide-hd,drive=disk0,bus=ide.0,bootindex=1 \
      -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${port}-:22" -device e1000e,netdev=net0 \
      -vga std -display "${APEX_WINVM_DISPLAY:-none}" -device qemu-xhci -device usb-tablet -rtc base=localtime \
      -qmp "unix:$qmp,server,nowait" -pidfile "$pidfile" -daemonize
    ;;
  stop)
    if ! running; then echo "not running"; exit 0; fi
    poweroff
    ;;
  status) running && echo running || echo stopped ;;
  *) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
