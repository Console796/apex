#!/usr/bin/env bash
# Report memory headroom and pressure. Run before and after any heavy build.
# Exit status: 0 healthy, 1 elevated pressure (some avg10 >= 10), 2 high pressure (>= 20).
# Physical path: a symlinked path to the checkout changes every absolute path in the build and rebuilds it all.
cd -P "$(dirname "$(readlink -f "$0")")/.."
source scripts/_memcap.sh
apex_mem_report
echo "suggested build jobs: $(apex_job_count)"
echo "top memory consumers:"
ps -eo rss,comm --sort=-rss | head -6 | awk 'NR>1 {printf "  %6d MiB  %s\n", $1/1024, $2}'
some=$(apex_psi some)
awk -v s="$some" 'BEGIN{exit !(s >= 20)}' && { echo "STATUS: HIGH pressure - do not start a build"; exit 2; }
awk -v s="$some" 'BEGIN{exit !(s >= 10)}' && { echo "STATUS: elevated pressure - use fewer jobs"; exit 1; }
echo "STATUS: ok"
