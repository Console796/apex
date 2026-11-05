# Agent instructions

## Building: memory is the constraint

This machine has ~15 GiB RAM and 12 cores. An uncapped `-j12` build runs it out of memory and can freeze
the whole desktop. A single compile or link above ~8 GiB is already dangerous.

**Always build and test through the wrapper scripts. Never run `cmake --build`, `ninja`, `make`, or
`ctest` directly.**

```
scripts/memcheck.sh                                   # headroom + pressure; run before any heavy build
scripts/build.sh [preset] [--target <name>]           # configure + build under a memory cap
scripts/test.sh [preset] [ctest args]                 # ctest under the same cap
```

Default preset is `linux-gcc-debug`. The scripts size `-j` from available memory, run the build in a
cgroup (`MemoryMax`, 8 GiB by default, hard-kills only the build; no `MemoryHigh` throttle unless
`APEX_MEM_THROTTLE=1`, because with swap off it stalls compiles instead of failing them), and limit concurrent links to 2.
Do not pass your own `-j` or bypass them with `APEX_NO_MEMCAP=1`.

### Rules

1. Run `scripts/memcheck.sh` before a full or first build. If it exits 2 (high pressure), do not start;
   wait or tell the user. Exit 1 means use fewer jobs (`APEX_JOBS=3`).
2. Prefer `--target <name>` over building everything when you only changed one target.
3. Run only one build at a time. Do not start a build while another (yours or a subagent's) is running;
   parallel agents must serialize builds.
4. If a build fails with `Killed`, signal 9, exit 137, or `bad_alloc`, it hit the memory cap. Do not retry
   unchanged. Retry with `APEX_JOBS=<fewer>` or a narrower target, and run `scripts/memcheck.sh` first.
5. Check pressure yourself any time the machine feels slow: `cat /proc/pressure/memory`
   (`some avg10` above ~10 means trouble) and `free -m`.
6. Build from `~/dev/apex`, never through the `~/projects/apex` symlink: a different source path rewrites
   every command and rebuilds the whole tree (the scripts resolve the physical path themselves).
7. Linking uses `ld.lld` automatically when installed (`APEX_PREFER_LLD`, on by default).

Tunables (env): `APEX_JOBS`, `APEX_JOB_MEM_MB` (default 1536), `APEX_MEM_MAX_MB` (8192, the hard cap),
`APEX_MEM_MAX_PCT` (unset; a percent of RAM instead), `APEX_MEM_HIGH_PCT` (85, of the hard cap: the job budget). CMake: `-DAPEX_LINK_JOBS=<n>`.
