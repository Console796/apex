# Shared helpers for the memory-capped build scripts. Source this file; do not run it.
#
# Tunables (environment):
#   APEX_MEM_MAX_MB    hard cap, MiB                           (default 8192)
#   APEX_MEM_MAX_PCT   hard cap as percent of MemTotal instead (overrides APEX_MEM_MAX_MB when set)
#   APEX_MEM_HIGH_PCT  job-count budget, percent of the hard cap (default 85). Set APEX_MEM_THROTTLE=1 to also
#                      apply it as MemoryHigh; off by default: with swap 0, throttling stalls compiles in
#                      reclaim for many minutes instead of failing.
#   APEX_JOB_MEM_MB    assumed memory per compile job, MiB    (default 1536)
#   APEX_JOBS          force the job count (skips the estimate)
#   APEX_NO_MEMCAP=1   run without the cgroup cap (not recommended)

apex_meminfo_kb() { awk -v k="$1:" '$1==k {print $2}' /proc/meminfo; }

# Hard and soft caps in KiB: APEX_MAX_KB, APEX_HIGH_KB. The hard cap never exceeds MemTotal.
apex_caps() {
  local total; total=$(apex_meminfo_kb MemTotal)
  if [[ -n ${APEX_MEM_MAX_PCT:-} ]]; then APEX_MAX_KB=$((total * APEX_MEM_MAX_PCT / 100))
  else APEX_MAX_KB=$((${APEX_MEM_MAX_MB:-8192} * 1024)); fi
  (( APEX_MAX_KB > total )) && APEX_MAX_KB=$total
  APEX_HIGH_KB=$((APEX_MAX_KB * ${APEX_MEM_HIGH_PCT:-85} / 100))
}

apex_psi() { # apex_psi some|full -> avg10 value
  awk -v t="$1" '$1==t {split($2,a,"="); print a[2]}' /proc/pressure/memory 2>/dev/null || echo 0
}

apex_mem_report() {
  local total avail
  total=$(apex_meminfo_kb MemTotal); avail=$(apex_meminfo_kb MemAvailable)
  printf 'memory: %d MiB available of %d MiB | PSI some avg10=%s full avg10=%s\n' \
    $((avail / 1024)) $((total / 1024)) "$(apex_psi some)" "$(apex_psi full)"
}

# Warn (and optionally refuse) when the machine is already under memory pressure.
apex_pressure_gate() {
  local some; some=$(apex_psi some)
  if awk -v s="$some" 'BEGIN{exit !(s >= 20)}'; then
    echo "WARNING: memory pressure is already high (PSI some avg10=$some). Wait for other work to" >&2
    echo "         finish, or build a single target with fewer jobs (APEX_JOBS=2)." >&2
    [[ ${APEX_FORCE:-0} == 1 ]] || { echo "Refusing to start; set APEX_FORCE=1 to override." >&2; return 1; }
  fi
}

# Echo a job count sized to the soft cap and to what is free right now.
apex_job_count() {
  if [[ -n ${APEX_JOBS:-} ]]; then echo "$APEX_JOBS"; return; fi
  local avail budget_kb jobs cores
  avail=$(apex_meminfo_kb MemAvailable)
  apex_caps
  budget_kb=$(( avail < APEX_HIGH_KB ? avail : APEX_HIGH_KB ))
  jobs=$(( budget_kb / (${APEX_JOB_MEM_MB:-1536} * 1024) ))
  cores=$(nproc)
  (( jobs > cores )) && jobs=$cores
  (( jobs < 1 )) && jobs=1
  echo "$jobs"
}

# apex_run_capped CMD...: run under a transient systemd scope with a memory cap.
# The kernel OOM-kills only inside the scope, never the desktop or the agent.
apex_run_capped() {
  local max_kb high_kb
  apex_caps
  max_kb=$APEX_MAX_KB high_kb=$APEX_HIGH_KB
  if [[ ${APEX_NO_MEMCAP:-0} == 1 ]] || ! command -v systemd-run >/dev/null \
     || ! systemd-run --user --scope --quiet true 2>/dev/null; then
    echo "WARNING: running WITHOUT a memory cap (systemd user scopes unavailable or disabled)." >&2
    "$@"; return
  fi
  echo "memory cap: MemoryMax=$((max_kb / 1024))M swap=0$([[ ${APEX_MEM_THROTTLE:-0} == 1 ]] && echo " MemoryHigh=$((high_kb / 1024))M")" >&2
  local props=(-p MemoryMax="${max_kb}K" -p MemorySwapMax=0)
  [[ ${APEX_MEM_THROTTLE:-0} == 1 ]] && props+=(-p MemoryHigh="${high_kb}K")
  systemd-run --user --scope --quiet --collect "${props[@]}" "$@"
}

# apex_oom_hint EXIT_CODE LOGFILE
apex_oom_hint() {
  local rc=$1 log=$2
  (( rc == 0 )) && return
  if (( rc == 137 )) || grep -qiE 'Killed|signal 9|terminated by signal|std::bad_alloc|out of memory|virtual memory exhausted' "$log" 2>/dev/null; then
    cat >&2 <<MSG

*** The build looks OOM-killed by the memory cap (exit $rc). ***
Do NOT just rerun with the same settings. Options:
  - rerun with fewer jobs:        APEX_JOBS=4 scripts/build.sh ...
  - build only the target needed: scripts/build.sh <preset> --target <name>
  - check headroom first:         scripts/memcheck.sh
MSG
  fi
}
