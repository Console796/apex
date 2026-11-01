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

**Not covered here.** Hammer cordon thickness/material measurements. The owner's in-game judgment on
the final tree. Gate 2's `p2ce.exe` launch was verified as a command line, not run (below).

# Windows real-tool gate (gate 2) — 2026-11-01

Tree `5f93819` plus the uncommitted working tree, built by `tools/winbuild.py --no-test` on the CI
machine (Windows 10 19045, i5-9300H, MSVC 2022, preset `windows-msvc-release`, no GPU needed).
Native tools from the Steam install `C:\Program Files (x86)\Steam\steamapps\common\Portal 2 Community
Edition` (`bin\win64\vbsp.exe`, `vvis.exe`, `vrad.exe`, `postcompiler\postcompiler.exe`; P2CE
`PatchVersion=0.0.8.2`, built Apr 23 2026). The chamber is `apex_tests -tc="*D17 scripted chamber*"` with
`APEX_CHAMBER_OUT` (63,352 bytes, SHA-256 `ed012c38...b2f9`), the same VMF the Linux gate uses.

The Linux test (`compile_chamber profiles through real QProcessRunner`, `tests/ui/compile_test.cpp`) is
`#ifdef __linux__`. It was run unmodified in logic on Windows from a copy edited on the CI machine only
(the repository is unchanged): the guard also accepts `_WIN32`; `bsppp` is included before `windows.h`
(its `BSPLump` enum collides with a Windows macro); `QTemporaryDir` uses the system temp folder instead of
`/tmp`; the postcompiler step may be Ok (it exists on Windows) instead of Skipped; and the entity
comparison sorts each entity's key/value pairs (see below). Command:

```bat
set APEX_P2CE_DIR=C:\Program Files (x86)\Steam\steamapps\common\Portal 2 Community Edition
set APEX_CHAMBER_VMF=C:\apex-ci\gate2\chamber.vmf
apex_ui_tests.exe -tc="compile_chamber*"
```

Result: 1 test case, 1,712 assertions passed. Times from the passing run (the earlier run was 4,281 /
3,237 / 2,276 / 2,757 / 3,123 ms):

| Variant | Profile | Snapshot write | Compile wall | Result |
|---|---|---|---|---|
| baseline | Fast | 3.3 ms | 2,988 ms | Ok; BSP copied to `maps`, 268,464 bytes, SHA-256 `050bb674...6c84`; vbsp alone 393 ms |
| final | Final | 3.7 ms | 3,249 ms | Ok; 268,464 bytes, SHA-256 `aa9aec16...676f`; no `LEAKED` line |
| leak | Fast | — | 2,283 ms to stop | vbsp Ok, `.lin` read (2 points), `vbsp.leak` diagnostic; job Failed after vbsp |
| cordon | Fast | 13.0 ms | 2,826 ms | Ok; 117,184 bytes; sealed cut |
| instance-addon | Fast | 5.2 ms | 2,911 ms | Ok; compiled into the addon's `maps` |

**Entity lump.** On Windows the Fast BSP's ENTITIES lump (after vrad) has the same 227 bytes and the same
key/value pairs as vbsp alone, but `worldspawn` lists `classname` first instead of last, so the unmodified
ordered comparison fails (one assertion). The test copy sorted each entity's pairs before comparing.

**Launch.** The game step's command was not run: the CI machine is driven over SSH without an interactive
desktop, and starting the game would need the BSP copied into the install. `p2ce.exe` exists at
`...\Portal 2 Community Edition\bin\win64\p2ce.exe` (388,608 bytes). The Windows build of
`apex_tests -tc="compile*,game launch*,addon campaign*"` (27 cases, 394 assertions) and `apex_ui_tests
-tc="compile panel*,compile QProcess*"` (9 cases, 132 assertions) pass, including the detached launch's
program (`game.toolPath("game")`, i.e. `bin\win64\p2ce.exe`) and arguments. The command line is
`p2ce.exe -dev -console -allowdebug -game "<install>\p2ce" +map <name> -condebug` (the default
arguments use `+map`).

**Repository test.** The same edits are now in `tests/ui/compile_test.cpp` (the case builds on Linux and
Windows, `bsppp` is included before `windows.h`, the temporary folder is the system default, the
postcompiler step may be Ok or Skipped, and entities compare with their key/value pairs sorted on every
platform). Linux (`tools/chamber_runner_check.py linux-asan`) passes with 732 assertions. The Windows
rerun of the repository's test is pending: the CI machine stopped answering during the rebuild.

**Not covered by this run.** A Windows launch that loads the map (owner playtest).
