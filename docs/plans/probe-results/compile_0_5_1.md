# 0.5.1 P0 probes (2026-10-27, Linux host)

## Process-tree termination (Bite 2, D1)

Probe: a Qt6 `QProcess` starting `sh -c 'sleep 301 & sleep 302'`, the shape of `p2ce.sh` (the shell
runs the real program as a child and does not `exec` it; see `$PROGRAM` in `p2ce.sh`).

| Termination | Survivors |
|---|---|
| `QProcess::kill()` | both `sleep` children survive |
| `setsid()` in `setChildProcessModifier`, then `QProcess::kill()` | both survive |
| `setsid()`, then `kill(-pid, SIGTERM)` | none |

Conclusion: `QProcess::kill()`/`terminate()` kill only the immediate child. The Linux runner must start
each step in its own session (`setChildProcessModifier(setsid)`) and terminate the process group
(`kill(-pid, SIGTERM)`, then `SIGKILL` after a grace period). D1 stands: this is done inside `QProcessRunner`,
so `compile/` is untouched. Windows (job object) is not yet probed; it belongs to the 0.5.1 Windows CI run.

## `.wc` files on this host

`hammer/cfg/cmdseq.wc` is the Strata KeyValues flavour (tab-indented, LF, keys `enabled`, `ensure_check`,
`no_wait`, `params`, `run`, `special_cmd`, `use_process_wnd`). `CmdSeqDefault.wc` is KeyValues with CRLF and
an extra `ensure_fn` key. Arguments use `$path\\$file` (a backslash joiner, even for Linux paths), the
postcompiler step passes `--propcombine`, and the final run step is `+map $file` with `no_wait 1`.
The `$path\$file` joiner has to be normalised at expansion (backslash to the host separator).
Not yet probed: toolpp `CmdSeq` round-trip (a test in 0.5.1), binary `.wc`.

## Instance path (D4, Bite 3)

Not yet measured. `tools/vbsp_equivalence.py` passes `-instancepath <root>/sdk_content/maps` and compiles from a
copy in `/tmp`; the runner passes `-instancepath` the same way (from `GameProfile::instanceSearchRoot()`)
unless a step already has it. The measurement runs once the real-tool CI stage exists.
