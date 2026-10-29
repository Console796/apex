# 0.5 release evidence: Linux real-tool gate — 2026-10-29

Tree: `1f89631` plus the uncommitted 0.5.7/0.5.8 work and the 2026-10-29 sprint changes (branch
`sprint/0.5-release`). Host: i7-13700KF, 24 threads, Ubuntu 26.04, preset `linux-gcc-release`.
Native tools from P2CE's `bin/linux64`. Command:

```bash
APEX_COMPILE_GATE_OUT=<dir> python3 tools/chamber_runner_check.py linux-gcc-release
```

The script now takes the preset as its argument (default `linux-asan`, which `tools/ci.py`'s
`compile-chamber` stage uses) and runs five variants of the D17 chamber through `QProcessRunner` and
`CompileJob`. Two runs (before and after the review fixes): 731 and 732 assertions passed. Times are the range over both.

| Variant | Profile | Snapshot write | Compile wall | Result |
|---|---|---|---|---|
| baseline | Fast | 0.54–0.58 ms | 3,790–4,384 ms | Ok; BSP copied; no `.lin`; entity lump equals vbsp-alone |
| final | Final | 0.53–2.26 ms | 4,395–4,495 ms | Ok; vrad ran HDR with bounces |
| leak | Fast | — | 1,407–1,531 ms to stop | vbsp Ok, `.lin` read (2 points), `vbsp.leak` diagnostic; job Failed after vbsp |
| cordon | Fast | 2.08–5.99 ms | 3,587–4,328 ms | Ok; sealed cut, no leak |
| instance-addon | Fast | 0.69–0.70 ms | 3,990–4,289 ms | Ok; created instance compiled into the addon's `maps` |

**Gate 1 (Linux).** Fast then Final run end-to-end on the 0.4 chamber, and a leaking variant (a point
entity in the void) leaves a path the overlay reader parses and a leak diagnostic. The owner's playtests on
`1f89631` remain the in-game judgment; this run did not launch the game (`runGameAfter = false`).

**Gate 3 (equivalence).** The baseline variant compiles the same VMF with vbsp alone (`-game`,
`-instancepath`, the D17 chamber) in a separate directory and compares the ENTITIES lumps with bsppp:
equal. This is the CI script's recipe re-run in-process, not the output file of
`tools/chamber_compile_check.py` itself.

**Gate 4 (measurements).**

- *Time to first step:* the snapshot write is 0.5–6 ms (largest with the cordon walls). It excludes the
  Run click's UI work and process start.
- *Fast vs vbsp alone:* 3,790–4,384 ms for Fast (vbsp, vvis -fast, vrad -fast, copy) against
  1,109–1,281 ms for vbsp alone. These are not like-for-like: Fast's extra time is vvis and vrad. On this one-light chamber
  each tool's run is dominated by start-up and loading the game's file system, so Final (4,395–4,495 ms) costs
  about the same as Fast. A larger map is needed to show the profiles diverging.
- *Panel cost while a log streams:* `compile panel runs Fast with an injected runner…` drains 10,000
  lines in 32–44 ms over three runs, with the worst 500-line refresh 0.65–2.5 ms (offscreen). The
  scripted runner delivers the burst before the panel drains it, so this measures the batched refresh,
  not frame pacing against a live process.

**Not covered here.** Gate 2 (Windows: native `.exe` tools and `p2ce.exe`): the real-tool test is still
`#ifdef __linux__`. Hammer cordon thickness/material measurements. The owner's in-game judgment on the
final tree.
