# 0.7.6 step 1: realtime diffuse probes

Work in progress, 2026-11-01. Scope: one camera-centred cascade, optional/default off,
GPU ray queries on SceneAccel, octahedral irradiance and depth moments, temporal
multi-bounce, viewport and render_pose controls, focused bounce/leak/lifetime tests.
No commits/pushes, no game-directory writes, no VM or GUI launches.

P0 completed first: see [realtime_gi_0_7_6_p0.md](realtime_gi_0_7_6_p0.md).
Existing unrelated edit in `src/editor/vrad_roundtrip.cpp` will be left untouched.

Implementation/verification findings will be appended as work proceeds. Existing
Lit output with the option off and baked/VRAD scales must remain unchanged.

## Implementation decisions (first compile)

- One `ProbeVolume` per SceneTarget, shared SceneAccel implementation/caster list;
  no independent geometry pipeline. Ray custom indices index a matching instance/
  primitive material table. Retained meshes carry optional material IDs.
- Default 32x32x16, 64 units, 256 probes/frame, 128 deterministic spherical rays per
  updated probe: 64 frames/full refresh. 8x8 octahedral irradiance and depth moments
  per probe, two 32 MiB buffers. Buffer maps use octahedral edge continuation for
  bilinear filtering; eight-probe trilinear gathering uses cubed Chebyshev visibility
  and a 2-unit normal offset. Back-face fraction rejects buried probes.
- Camera grid snaps to spacing. Grid shifts, geometry/material/light changes clear
  history. Full recenter/reset is intentionally simple; scrolling reuse/relocation
  and camera-priority scheduling are later work. Previous-frame maps are separate
  from the destination, including when only part of the volume updates.
- A conservative `waitIdle` precedes volume mutation. This makes lifetime safe across
  frame command-buffer slots but serializes enabled rendering; this step establishes
  correctness, not an asynchronous GPU update/refit design. Off does not take this path.
- Static/None indirect modes do not inject, Dynamic (2) does, independently of direct
  mode. Sky misses/sky faces inject the ambient emitter and replace CPU sky sampling
  only while probes are active. Outside volume coverage returns black. Existing fill
  and AO controls remain independent.
- Added viewport flag/menu/pane label and `render_pose --indirect off|probes
  --indirect-frames N`; independent poses reset history. RT-less render_pose requests
  fail explicitly; the editor capability-gates the pass. No fallback added.

Build is using the ordinary CMake/Ninja graph with `-j3`; regeneration succeeded.
No FetchContent settings were changed and no Embree translation units were rebuilt.

The report map `/home/taylorg/maps/apex/test1.vmf` explicitly sets its `light_rt`
and `light_rt_spot` `_indirectmode` to 1 (Static). Under the requested game rule
these lights do not inject into realtime probes. The report comparison can measure
that behaviour, but cannot demonstrate ceiling bounce from those authored static
lights without a separately labelled modified input; the implementation will not
silently promote them to Dynamic. This is a material limitation for the report goal.

## First GPU evidence

`QT_QPA_PLATFORM=offscreen APEX_VK_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1
build/linux-clang-release/tests/apex_tests --test-case='render: realtime probe*,render: probe reflectivity*'`
passed 3 cases / 418 assertions on llvmpipe. A spotlight faces the floor in two
sealed rooms separated by an actual 16-unit box wall. The ceiling test points
have exactly zero CPU direct irradiance. 25-point mean linear surface luminance
(albedo 0.65) rises from 0 off to **0.04215** with probes after 16 frames.
Reset/repeat output is byte-identical; Static and None injection produce zero;
reducing reflectivity reduces bounce. Maximum of near-wall and room-interior dark
patches, relative to the lit patch:

| Wall/grid phase, units | Bright | Dark | Dark/bright |
| --- | --- | --- | --- |
| 0 | 0.042150 | 0.00006317 | 0.1499% |
| 16 | 0.042997 | 0.00002086 | 0.0485% |
| 32 | 0.046456 | 0.000001653 | 0.00356% |

Threshold is 5%; these results are evidence for this sealed fixture, not a general
no-leak guarantee. No validation errors or warnings, including destruction after
empty topology, TLAS refit, buffer resize and camera-grid recenter.

Further tests added for budgeted history preservation, sky injection, camera
rebasing, renderer enable/disable, Fullbright/Baked isolation and viewport JSON.
History ownership moved into SceneTarget so closing a viewport releases its maps
and TLAS rather than retaining 64 MiB per retired viewport in Renderer.
Viewport tick continues rendering while the enabled ray-query volume accumulates.

## Report scene capture

`apex_render_pose --vmf /home/taylorg/maps/apex/test1.vmf --game "$APEX_P2CE_DIR"
--width 320 --height 180 --mode lit --pose p1 -1135.97 -1135.97 64.03 -9.13 34.18 0`
with `--indirect probes --indirect-frames 64` wrote
`build/probes/realtime_gi_0_7_6/probes/p1.png`; the off run wrote
`build/probes/realtime_gi_0_7_6/off/p1.png`. These are byte-identical (zero differing
channels), consistent with the report's authored Static modes. Probe capture ran
with validation + synchronization validation and printed no validation diagnostics.

Attempted `--mode vrad --vrad-flags '-final -threads 3' --vrad-scale 1` with the same
pose and dimensions, output `build/probes/realtime_gi_0_7_6/vrad`. Stopped after
more than six minutes: the round trip had written its temporary VMF/gameinfo but
VBSP had not produced a BSP, and the blocking runner exposed no intermediate log.
No VRAD image/ratio is claimed. Existing 0.7.5 findings also establish that VRAD
ignores test1's light_rt/light_rt_spot (only texlights bake), so such a ratio would
not isolate probe approximation quality. Raw attempt: `/tmp/apex-gi-report-vrad.log`.

## Integration and capability evidence

Focused sync-validation run: **6 cases, 544 assertions passed**, zero errors or
warnings (`/tmp/apex-gi-focused3.log`). Normal Renderer::prepare/record renders a
ceiling patch outside the spot cone: mean display channel value **0 -> 0.0604243**.
Enable -> disable restores the exact off bytes. Baked and Fullbright option-on
images equal their option-off bytes. Captures (32x32, converted losslessly to PNG):

- `build/probes/realtime_gi_0_7_6/fixture/ceiling-off.png`
- `build/probes/realtime_gi_0_7_6/fixture/ceiling-probes.png`

```sh
QT_QPA_PLATFORM=offscreen APEX_VK_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1 APEX_GI_CAPTURE_DIR=build/probes/realtime_gi_0_7_6/fixture build/linux-clang-release/tests/apex_tests --test-case='render: realtime probe*,render: probe reflectivity*,render: probe budgets*,render: realtime indirect*,viewport JSON*'
QT_QPA_PLATFORM=offscreen APEX_VK_VALIDATION=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_profiles VK_LAYER_SETTINGS_PATH=/tmp/apex-no-rt build/linux-clang-release/tests/apex_tests --test-case='render: realtime probe*,render: probe budgets*,render: realtime indirect*'
```

The existing temporary 0.7.3 Khronos profile hides RT extensions. Context prints
`ray_query false, accel_struct false`; the normal renderer accepts the requested
option and leaves output unchanged. GPU-only cases skip explicitly. **4 cases,
28 assertions passed**, no validation errors/warnings (`/tmp/apex-gi-no-rt.log`).
The editor disables the menu entry without capability and labels a saved enabled
flag as unavailable. No no-RT illumination fallback was implemented.

Final changes cover full Source-world ray distance (262144 units, beyond the
±65536 cube diagonal) and cap dispatch groups at Vulkan's guaranteed 65535 limit.

## File map for review

| Files | Change |
| --- | --- |
| `src/render/probe_volume.{hpp,cpp}` | Per-viewport SceneAccel, hit metadata, settings validation, compute pipeline/descriptors, ping-pong maps, budgets/reset/synchronization. |
| `src/render/shaders/probe_update.comp.glsl` | 128-ray updates, shadowed direct injection, Dynamic-only modes, previous-frame diffuse transport, sky, depth moments/temporal blend/buried rejection. |
| `src/render/shaders/probes.inc` | Octahedral decode/filtering, eight-probe depth visibility and normal-offset sampling. |
| `src/render/renderer.{hpp,cpp}` | Default-off option/settings; capability-checked update before scene rendering; independent-capture reset. |
| `src/render/scene_target.{hpp,cpp}` | Own history through viewport lifetime. |
| `src/render/gpu_lighting.hpp`, `shaders/frame.inc` | Appended indirect-volume ABI, existing offsets unchanged. |
| `src/render/gpu_material.hpp`, `shaders/material.inc` | Appended linear bounce reflectivity/classification ABI. |
| `src/render/shaders/lighting.inc`, `shaders/brush_textured.frag.glsl` | Shared direct model without a frame dependency for ray hits; Lit-only indirect seam and sky replacement. |
| `src/render/bake_setup.{hpp,cpp}` | Extract exactly the existing reflectivity override/clamp operation for reuse; baker arithmetic unchanged. |
| `src/lighting/cpu_accel.hpp`, `src/render/render_mirror.{hpp,cpp}` | Retain optional material IDs; upload baker reflectivity (base VTF, even when preview differs), classify sky/tools, pass the option. |
| `src/render/shader_blobs.cpp`, `CMakeLists.txt` | Register the pass and shader. |
| `src/viewport/viewport.{hpp,cpp}`, `src/ui/viewport_pane.{hpp,cpp}` | Saved default-off flag, menu/capability/HUD, idle accumulation. Other concurrent UI edits are not part of this slice. |
| `tools/render_pose/main.cpp` | `--indirect off|probes`, positive bounded `--indirect-frames N`, per-pose accumulation/reset and explicit unavailable-device error. |
| `tests/render/probe_volume_test.cpp`, `tests/render/shaders/probe_sample.comp.glsl` | Deterministic bounce, thin walls, modes/materials, partial updates, sky/rebasing, lifetime and normal renderer isolation. |
| `tests/render/scene_target_test.cpp` | Update sample ABI size for appended material field. |
| `tests/viewport/viewport_test.cpp`, `tests/CMakeLists.txt` | Option JSON compatibility, register tests and include dependencies. |
| This report and `realtime_gi_0_7_6_p0.md` | Early measurements, decisions, limitations, reproduction and verification. |

## Deliberately deferred / uncertainties

- No cascades, relocation, scrolling reuse, A/B polish or no-RT illumination fallback.
- Hardware GPU timings/quality remain unmeasured here; llvmpipe is the available device.
- Synchronous wait/refit, 64-frame default refresh and reset-on-grid-shift can be slow
  while flying/dragging. No claim of production realtime performance from this slice.
- Octahedral finite-ray visibility is approximate; the fixture's low leakage does not
  establish arbitrary wall/corner behaviour. Outside coverage is dark.
- No report-scene VRAD ratio (blocked native round trip and mismatched emitters).
- Existing ambientFill/AO remain independently selectable; they are not retired or retuned.
- No bake numerics or Baked/VRAD display scales changed by this slice; no goldens updated.

## Final verification

Ran the requested chain, with the inherited APEX_P2CE_DIR:

```sh
cmake --build build/linux-clang-release -j3 && python3 tools/check_layers.py && QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux-clang-release --output-on-failure -j4
```

Build **passed**, layer check **passed (zero violations)**. No dependency/FetchContent
settings were changed; existing Embree archives were reused (only their normal
post-build symbol-check scripts ran). Full ctest reached **1061/1062 completed**;
it is **not a green full-suite result**:

- Existing `Actions registry has stable ordered unique ids and complete metadata`
  fails: expected 87 entries, actual 88. The concurrent command-palette work adds
  `app.command_palette` to `src/editor/actions.cpp`; this slice does not change
  that registry or its count assertion.
- Existing `a sealed lit room compiles through the real tools and its faces map
  back onto the document` remained in its blocking native round trip after the
  other cases completed. Stopped ctest with SIGINT rather than leave that native
  process running. It has the same unresolved VBSP behaviour as the report attempt.
- Seven opt-in measurement cases skipped normally. The corpus P0 was separately
  run explicitly as recorded in the P0 report. All new GI cases passed in ctest.

Full log: `/tmp/apex-gi-ctest.log`. Final shader/ABI state also passed focused
synchronization validation again: **6 cases / 544 assertions**, no validation
errors/warnings (`/tmp/apex-gi-final-sync.log`). `git diff --check` passes.

Angular irradiance integration uses the existing Source `surfaceResponse`; hit
radiance uses linear reflectivity/pi and the sky emitter is normalized by
6/(5*pi). This is an intentionally Source-response diffuse approximation, not a
VRAD parity fit. No display scales were tuned to compensate.

No commit or push was made. Concurrent command-palette/UI, build-script and
other-engineer scale edits were preserved; their diffs are outside this file map.
