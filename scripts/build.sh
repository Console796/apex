#!/usr/bin/env bash
# Memory-capped configure + build. The ONLY supported way to build on this machine.
#
#   scripts/build.sh [preset] [extra `cmake --build` args...]
#   scripts/build.sh linux-gcc-debug --target apex_tests
#
# Default preset: linux-gcc-debug. Tunables are documented in scripts/_memcap.sh.
set -uo pipefail
# Physical path: a symlinked path to the checkout changes every absolute path in the build and rebuilds it all.
cd -P "$(dirname "$(readlink -f "$0")")/.."
source scripts/_memcap.sh

preset=linux-gcc-debug
if [[ $# -gt 0 && $1 != -* ]]; then preset=$1; shift; fi

apex_mem_report
apex_pressure_gate || exit 75
jobs=$(apex_job_count)
echo "preset: $preset | jobs: $jobs"

log=$(mktemp -t apex-build.XXXXXX.log)
trap 'rm -f "$log"' EXIT

run() {
  if [[ -t 1 ]]; then apex_run_capped "$@"; else apex_run_capped "$@" 2>&1 | tee "$log"; fi
}

# Configure when the tree is missing or stale (cheap no-op otherwise).
run cmake --preset "$preset"; rc=$?
if (( rc != 0 )); then apex_oom_hint "$rc" "$log"; exit "$rc"; fi
run cmake --build --preset "$preset" -j "$jobs" "$@"; rc=$?
apex_oom_hint "$rc" "$log"
exit "$rc"
