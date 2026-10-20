#!/usr/bin/env bash
# Headless launcher for the Windows CI guest (plan 0.1, D23). Same hardware as
# $APEX_WINVM_DIR/start.sh, but no window, SSH forwarded to 127.0.0.1:$APEX_WINVM_SSH_PORT,
# and a QMP socket for graceful shutdown.
#   tools/winvm.sh start|stop|status
# Set APEX_WINVM_DISPLAY=gtk to get a window (for setup/debugging).
set -euo pipefail
dir="${APEX_WINVM_DIR:-/mnt/hdd/winvm}"
port="${APEX_WINVM_SSH_PORT:-2222}"
pidfile="$dir/ci.pid"
qmp="$dir/ci.qmp"

running() { [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; }

case "${1:-}" in
  start)
    if running; then echo "already running"; exit 0; fi
    cd "$dir"
    qemu-system-x86_64 -name win10-ci -machine q35,accel=kvm \
      -cpu host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time \
      -smp "${CPUS:-6}" -m "${RAM:-8G}" \
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
    printf '{"execute":"qmp_capabilities"}\n{"execute":"system_powerdown"}\n' | \
      timeout 5 socat - "UNIX-CONNECT:$qmp" >/dev/null || true
    for _ in $(seq 60); do running || exit 0; sleep 2; done
    echo "guest did not power down in 120s; killing" >&2
    kill "$(cat "$pidfile")"
    ;;
  status) running && echo running || echo stopped ;;
  *) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
