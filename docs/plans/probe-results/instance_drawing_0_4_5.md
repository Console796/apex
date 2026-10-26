# Instance drawing slice (2026-10-26)

D7 drawing uses separate immutable Documents: the cached source template and a copy with
`fixedInstanceKeys()` applied. Neither is attached to an EditorSession. Geometry stays local;
placements use composed `document::entityTransform(document::entityPose(...))` matrices.
Nested replacement values and names are fixed before parsing the next level's context.

`nInstanceVisibility` is 0 hidden, 1 tinted (green), 2 normal. The checked-in Hammer saves,
including `render_map_nested_groups_saved.vmf`, put value 2 in `viewsettings`.
The numeric meanings are corroborated by
[srctools' StrataInstanceVisibility enum](https://github.com/TeamSpen210/srctools/blob/master/src/srctools/vmf.py#L318-L322).
No new Hammer GUI probe was run. The existing session grid setter is outside undo history;
instance visibility follows that convention. Document display-setting overrides are merged into
`viewsettings` on write, survive undo, and notify observers without an undo entry.

## UI handoff

- `session.setConnectionContext(mapPath, profileMapsRoot)` supplies resolution roots and expands contents.
- `session.instances().contents()` is the document-only input for the mirror.
- `mirror.setInstanceContents(session.instances().contents(), session.instanceVisibility())` synchronizes drawing.
  The only UI change is this one call in `ViewportArea::updateHighlight()`.
- `session.instanceVisibility()` / `session.setInstanceVisibility(document::InstanceVisibility)` expose the per-file preference.
- `session.instances().dependencies()` maps canonical template paths to top-level instance IDs for the watcher.
- `session.invalidateInstance(path)` reloads, re-expands, updates picking and notifies observers. Its
  `InstanceReload` contains affected top-level `users` and an optional reload `problem`.
- `session.instances().problems()` returns unresolved/load/cycle/depth/reload diagnostics.
- Standalone users can call `InstanceTemplates::invalidate(path)` (returns `Result<Template>`), or
  `InstanceExpansion::invalidate(path)` followed by `update(document, mapPath, profileMapsRoot)`.
  Failed parsing preserves the last good cached template.
- `PickIndex::setInstanceContents(contents, visibility)` is available outside a session. Ray and
  frustum results contain only the parent instance ID; content hits have no document SideId.

## Measurements

GCC Debug ASan/UBSan, llvmpipe LLVM 21.1.8, Vulkan validation enabled, two worker threads.
All 95 corpus maps with resolvable contents were checked against an independent recursive source
solid count: 2,568 visible template solids, including nested templates. Hidden wrappers retain the
existing mirror's visibility semantics. Authored fixture tests also run without the corpus.

Largest by expanded solid count:
`sdk_content/maps/instances/tube_elevator/turbine_elevator_connected_entry.vmf`.
The measurement uses fresh template, material and model caches; the filesystem cache is warm.
Parent parsing and VFS initialization precede the timer. It includes the parent mirror, asset
loading, settling, one Vulkan frame, and readback; helper classes are not supplied in this workload.

| Metric | Result |
|---|---:|
| Expanded placements / template solids | 6 / 233 |
| Expansion | 726.772 ms |
| Expansion through completed draw and readback | 1,468.61 ms |
| Mirror geometry buffer payload | 3,233,544 bytes (3.08 MiB) |
| Instance geometry buffer payload | 3,233,368 bytes |
| Uploaded mirror texture mip payload | 30,664,220 bytes (29.24 MiB) |
| Fixed-copy node block storage | 5,916,627 bytes (5.64 MiB) |

GPU totals exclude the shared checkerboard, staging ring, per-frame transform buffers, driver/VMA
allocation overhead and render target. Fixed-copy block storage excludes Document/index/container
storage and shared source text; it is not a whole-process RSS measurement.

GPU meshes, models and textures are shared across placements with the same template/fixup copy.
Different fixup variants have separate child mirrors and allocations, matching the existing
mirror's ownership of solid/material batches. The largest map has 233 unique solid meshes, so
there is no solid-sharing saving on that map. A fixture with three placements has 12 drawn solids
and eight unique solid meshes. Reloads compare old/new immutable node snapshots: an entity-only
change submits no solid mesh jobs, and a one-solid change remeshes once for both placements.
Structural edits that shift template-local node IDs can remesh additional solids.

## Checks and diagnostic

- `ASAN_OPTIONS=detect_leaks=0 cmake --build build/linux-asan -j6 --target apex_tests` passed.
- The same build with `--target apex_tests apex_ui` passed, including the one-line UI handoff.
- With `APEX_P2CE_DIR=` (fixture-only), `ASAN_OPTIONS=detect_leaks=0`,
  `UBSAN_OPTIONS=halt_on_error=1`, `APEX_VK_DEVICE=llvmpipe`, `APEX_VK_VALIDATION=1`:
  `build/linux-asan/tests/apex_tests -tc='*nstance*'` passed 29 cases / 6,932 assertions;
  the two corpus cases report their opt-out.
- With the corpus enabled and the same Vulkan settings, `-tc='Instance corpus*'` passed
  1 case / 1,233 assertions. It emits an existing UBSan alignment diagnostic at VFS teardown.
- With the corpus enabled and `UBSAN_OPTIONS=halt_on_error=1`, the focused regression command below
  passed 71 cases / 559,559 assertions. Vulkan validation reported no errors.
- `python3 tools/check_layers.py` and `git diff --check` passed.

Exact focused regression command:

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 APEX_VK_DEVICE=llvmpipe APEX_VK_VALIDATION=1 build/linux-asan/tests/apex_tests -sf='*/tests/editor/pick_index_test.cpp,*/tests/editor/pick_model_test.cpp,*/tests/editor/editor_session_test.cpp,*/tests/render/render_mirror_test.cpp,*/tests/render/model_render_test.cpp' -tce='Instance corpus*'
```

The corpus diagnostic is a packed `vpkpp::VPK::Footer2` containing `std::vector` members
(`build/linux-asan/_deps/sourcepp-src/include/vpkpp/format/VPK.h`, pack 1 around lines 28–60).
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` makes both the new corpus test and the unchanged
`-tc='VFS resolves real P2CE loose and Portal 2 VPK material'` fail in the same destructor at
`tests/assets/vfs_test.cpp:173`. The stack is `Footer2::~Footer2` → `VPK::~VPK` → `Vfs::~Vfs`.
This dependency issue is neither suppressed nor changed by the drawing slice. Corpus assertions
pass, but the real-VPK corpus run is not sanitizer-clean.
