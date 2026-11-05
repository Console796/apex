#!/usr/bin/env bash
# Memory-capped ctest.   scripts/test.sh [preset] [extra ctest args...]
set -uo pipefail
# Physical path: a symlinked path to the checkout changes every absolute path in the build and rebuilds it all.
cd -P "$(dirname "$(readlink -f "$0")")/.."
source scripts/_memcap.sh
preset=linux-gcc-debug
if [[ $# -gt 0 && $1 != -* ]]; then preset=$1; shift; fi
apex_mem_report
apex_pressure_gate || exit 75
apex_run_capped ctest --preset "$preset" -j "$(apex_job_count)" "$@"
