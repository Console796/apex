# Tests, tools and documents

Three things sit beside the code: the tests, the helper scripts, and the design documents. This page
says what each contains and how to use it.

## Tests

### Layout

[`tests/`](../../tests/) mirrors `src/`. A source file in `src/editor/` is tested by a file in
`tests/editor/`, named `<source>_test.cpp`. Some tests cover several sources, and some have no
single counterpart. Browse the directory.

| Directory | Covers |
|---|---|
| `tests/core/` | KV1, KV3, JSON, handles, paths, strings, job pool, logging. |
| `tests/geometry/` | Solid geometry, hull, primitives, face UVs, BVH, and the geometry corpus test. |
| `tests/document/` | VMF round-trips, transactions, instances, connection codec, solid rebuilds, cordons. |
| `tests/compile/` | Sequences, variables, runner, diagnostics against the golden logs in `tests/compile/logs/`, `.lin` and `.prt` readers, snapshots. |
| `tests/lighting/` | The light model, sky visibility probes, and the specular terms. |
| `tests/assets/` | VFS, game profiles, VMT, VTF, FGD, indexes, caches, model baking, addons and addon authoring. |
| `tests/editor/` | Tools, selection, transforms, gizmos, clipboard, groups, visgroups, autosave, problems, find and replace. Most of the suite lives here. |
| `tests/render/` | Renderer, context, view maths, thumbnails, the `Lit` passes. Uses golden images in `tests/render/golden/`. |
| `tests/viewport/` | Layouts, workspaces, cameras, picking maths. |
| `tests/ui/` | Qt shell tests. Built into the separate `apex_ui_tests` target. |
| `tests/fuzz/` | libFuzzer targets for KV1, VMF and the hull. Built with `APEX_BUILD_FUZZ`. |
| `tests/fixtures/` | Apex-authored input files. See the rules below. |

The entry point is [`tests/main.cpp`](../../tests/main.cpp), which uses doctest. The target list is
in [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt).

### Three kinds of test

- **Unit tests** use the fixtures in `tests/fixtures/`. They run everywhere.
- **Corpus tests** read real P2CE maps from `APEX_P2CE_DIR`. Their names often contain `corpus`.
  Without that variable they skip. The corpus is never committed.
- **GPU tests** need a Vulkan device. CI runs them on lavapipe, which is required, and on RADV when
  an AMD GPU is present.

### Running tests

```bash
ctest --preset linux-asan                                   # everything registered
build/linux-asan/tests/apex_tests -tc='*face*'              # one area by test-case name
```

The full ASan suite takes about 15 minutes, so run only the tests your change touches and leave the
full run to CI. [../development.md](../development.md) section 3 has the UI-test command, which must
run offscreen and away from your desktop session.

### Fixtures

Files in `tests/fixtures/` must be written by Apex. Text fixtures must have an `Apex-authored`
marker in their first lines. Never add Valve or Strata content. The
[fixture README](../../tests/fixtures/README.md) gives the details. Two subdirectories are special:

- `tests/fixtures/topology/` holds solids that the same-topology write path rejects. They are kept
  apart from `vmf/`, because every identity sweep expects `vmf/` fixtures to round-trip.
- `docs/plans/probe-results/` holds files written by Strata Hammer itself, plus the probe inputs.
  They are kept out of `fixtures/` because they are not Apex-authored. See
  [its README](../plans/probe-results/README.md).

## Tools

[`tools/`](../../tools/) holds scripts and small programs. The ones a newcomer uses:

| Script or program | What it does |
|---|---|
| [`ci.py`](../../tools/ci.py) | The local CI gate. Modes: full, `--fast`, `--tests`. Writes `build/ci/<build>/report.md`. |
| [`ci_top.py`](../../tools/ci_top.py), [`ci_status.py`](../../tools/ci_status.py), [`ci_history.py`](../../tools/ci_history.py) | Watch a running gate, print its summary, and show past runs. |
| [`check_layers.py`](../../tools/check_layers.py) | Enforces the include rules between layers and the `processModelData` ban. |
| [`check_licenses.py`](../../tools/check_licenses.py) | Checks that dependencies are compatible with MIT and GPLv3-or-later. |
| [`install-hooks.sh`](../../tools/install-hooks.sh) | Installs `ci.py --fast` as the `pre-push` hook. |
| [`package.py`](../../tools/package.py) | Builds the Linux `.tar.gz` or Windows `.zip` from a release build. |
| [`vmf_dump/`](../../tools/vmf_dump/) | Dumps object IDs, side IDs and plane strings of a VMF as TSV, for the srctools cross-check. Not shipped. |
| [`replane/`](../../tools/replane/) | Writes a re-planed copy of a VMF, used by the manual `vbsp_equivalence.py` gate. |
| [`pie_probe/`](../../tools/pie_probe/) | Plan 0.3 probe for the pie-menu popup over a native child window. Has its own CMake file. |
| [`instance_collapse/`](../../tools/instance_collapse/) | Tools-only driver for `instance_collapse_crosscheck.py`. |
| `srctools_crosscheck.py`, `vbsp_equivalence.py`, `chamber_compile_check.py`, `chamber_runner_check.py`, `hammer_wine_check.py` | Cross-checks against other tools. `hammer_wine_check.py` opens and re-saves maps in real Strata Hammer under Wine. |
| `make_probe_vmfs.py`, `gen_angle_fixture.py` | Generate the probe and angle fixtures. |
| `capture_compile_logs.py` | Compiles broken variants of the chamber with the real P2CE tools and stores their logs as fixtures for `compile/diagnostics`. Needs `APEX_P2CE_DIR`. |
| `light_probe.py`, `light_parity.py` | Lighting probes. `light_probe.py` measures how native VRAD turns light keys into worldlights. `light_parity.py` checks irradiance against native VRAD (plan 0.6, D3). Both need `APEX_P2CE_DIR` and write only to `/tmp`. |
| `embed_spirv.py` | Turns compiled shaders into headers. See [repo-map.md](repo-map.md#where-generated-code-comes-from). |
| `winvm.sh`, `winvm-lock.sh`, `winvm-unlock.sh`, `winvm_lock.py` | Start, lock and unlock the Windows VM used for the MSVC build and tests. Setup is in [../development.md](../development.md) section 6. |
| `ci_history.py` | Stage timing history and ETA prediction, shared by `ci.py` and `ci_top.py`. |
| `test_ci.py`, `test_top.py` | Regression tests for the CI orchestration, and `test_top.py`, a terminal monitor for a direct run of `apex_tests` or `apex_ui_tests`. |
| `input_trace_summary.py` | Summarises an `APEX_INPUT_LOG` input trace (JSONL). |
| [`vtf_stats/`](../../tools/vtf_stats/) | Prints a VTF's format and the per-face mean and max of each channel. |
| [`render_pose/`](../../tools/render_pose/) | Renders one model pose for inspection. Links the renderer, so it is built with the project. |

## Documents

[`docs/`](../) is the design record. The status of each file tells you how far to trust it.

| File | Status | Use it for |
|---|---|---|
| [`roadmap.md`](../roadmap.md) | Living | The milestones, their order, and the versioning rules. The authoritative answer to "what's next". |
| [`research.md`](../research.md) | Background | How Strata differs from Source, prior art, options considered, and the open questions. |
| [`development.md`](../development.md) | Living | The procedure: host setup, building, testing, the CI gate and the Windows VM. |
| [`plans/0.1.md`](../plans/0.1.md) to [`plans/0.6.md`](../plans/0.6.md) | Record | Each milestone's decisions (`D<n>`), its sub-milestones, what is out of scope, and the things that will bite. Read section 2 of each for the seams. 0.5 and 0.6 are still open, and their plans have progress notes. |
| [`plans/addons.md`](../plans/addons.md) | Research and plan | Strata addon interop: what Strata does, facts still to probe, and draft decisions. |
| [`plans/probe-results/`](../plans/probe-results/) | Evidence | Probe VMFs and the notes on them. The [README](../plans/probe-results/README.md) indexes them. Includes the 0.5 compile and cordon notes, the 0.6 lighting notes (`lighting_0_6_*.md`, `light_helpers.md`, `material_normals_0_6_5.md`, `captures_test1_0_6_5.md`), and the Hammer findings from 0.3 and 0.4. |

A plan records what was decided at the time. Later code may have moved on. If a plan and the code
disagree, the code is the current state, and the plan explains why it was written that way.

The other docs in the repo are [`../../README.md`](../../README.md) (what Apex is),
[`../../CONTRIBUTING.md`](../../CONTRIBUTING.md) (rules) and [`../../THIRD_PARTY.md`](../../THIRD_PARTY.md)
(third-party components and licences).
