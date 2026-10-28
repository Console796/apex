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

## Instance path (D4, Bite 3), measured 2026-10-27

Map: the D17 chamber plus one `func_instance` with `file "apex_inst.vmf"` (Apex-authored instance with an
`info_target` named `apex_inst_marker`), compiled with native `vbsp -game <p2ce> [-instancepath X] with_inst.vmf`
from a copy in a scratch directory (never in the install).

| Layout | Result |
|---|---|
| (a) instance file beside the VMF, `-instancepath <root>/sdk_content/maps` | exit 0; marker present in the BSP |
| (b) no instance file anywhere | **exit 1**, `Could not open instance file apex_inst.vmf` (a hard error, not a warning) |
| (c) instance only under `-instancepath <dir>` | exit 0; marker present in the BSP |

Conclusions: vbsp resolves a relative `file` from the VMF's folder and then from `-instancepath`. A missing
instance fails the compile outright, so Bite 3's "compiles with a warning and no instance" does not happen; the failure
is loud, and the diagnostic rule is `Could not open instance file <file>`. A cache-directory snapshot therefore
works only if the instance search reaches the source map's folder. Plan for 0.5.2/0.5.4: the snapshot step passes
`-instancepath` as the source VMF's folder when the map is saved (instances beside the source), falling back to
`GameProfile::instanceSearchRoot()`; vbsp takes one `-instancepath`, so a map whose instances live in both needs the
snapshot to copy the source folder's instance files into the work directory (or use the collapsed snapshot, 0.4 D9).
Not measured: whether repeating `-instancepath` is accepted.

## Real-tool gate

`tools/chamber_runner_check.py` generates the D17 chamber and runs the `compile_chamber` test: Fast through
`QProcessRunner` + `CompileJob` on Linux. Passed first run: vbsp, vvis and vrad Ok, postcompiler skipped (no Linux
build), BSP copied to a temporary `maps`, no `.lin`.
