# 0.7.5 slice B: VRAD view in the editor, A/B compare, luxel density

Status: slice B done (wiring, compare, density, tests). Written early; updated as the work lands.

## Design

- `viewport::DisplayMode::Vrad` renders through `render::DisplayMode::Baked` (same shader path); only the
  lightmap set bound on the mirror differs. `kDefaultVradScale = 1.0` in `renderer.hpp`.
- `ui::VradController` (GPU-free, so testable offscreen with a fake `ProcessRunner`) owns the `VradRoundTrip`:
  starts on entering the mode (when nothing was attempted, or the last run was cancelled), `Re-run VRAD`
  restarts (cancelling a run in flight), leaving the mode cancels a run in flight. A finished result stays
  until the next run replaces it; an edit after it only labels it STALE in the HUD (faces keep their compiled
  fragments until the re-run). Failure shows the first log line in the HUD and the tool's log tail in the tooltip.
  Settings: QSettings `vrad/flags` (default `-noextra`), `vrad/vis`, `vrad/displayScale`; menu "VRAD settings...".
- `GpuHost` holds the Apex `BakeController` and the VRAD `BakedLightmaps`; `selectLightmaps(source, density)`
  swaps which set the mirror draws. One mirror means one set: the active lightmapped pane picks the source and a
  pane in the other mode says "showing the ... lightmaps (active pane)" in its HUD.
- The BSP load (`loadVradLightmaps`) runs on the GUI thread when the compile finishes (the compile itself is
  background); fine for the sizes measured, a job-pool move is possible later.

## Compare (D7)

A split wipe needs two meshes of the same solids in one frame (lightmap UVs are per-vertex and differ per source),
and the mirror holds one set, so a split is disproportionate. Done as a toggle: per-viewport
"Compare light source > A/B with X" then "Swap A/B" flips the viewport between its mode and X (Lit, Baked, VRAD);
the pane title shows the mode now showing and what a swap switches to. No hotkey (Wayland embedded windows get
no key events; the pane menu is the entry). Offscreen: `apex_render_pose --compare A,B[,split]` renders both
sources and writes `N-compare.png`, A left of the split (fraction of width, default 0.5), B right, with a white divider.

## Luxel density (D7)

`render::densityRadiance(atlas, target=16)` replaces the radiance with a checker, one cell per luxel, hue by
the face's units per luxel against the target (green within 10%, red coarser, blue finer); same layout as radiance
so it uploads through the existing path. Overlay flag `luxelDensity` (Baked and VRAD), legend appended to the HUD.
Not drawn in Lit (no atlas). The checker is multiplied by the surface albedo like any baked radiance. `--density`
in render_pose.

## Tests and evidence

- `tests/ui/vrad_view_test.cpp` (offscreen, scripted runner, fake loader): start on entry, one publish, STALE
  label after an edit with the old image kept, Re-run VRAD clears it, leaving the mode cancels, failure text with
  log tail, flags/vis/scale settings reach the tools.
- `tests/render/lightmap_atlas_test.cpp`: compiled atlas keeps fragments covering no more than the face (strict
  baker check rejects the same shrunk fragment; a spilling fragment is rejected); density layout/hue/determinism.
- `tests/viewport/viewport_test.cpp`: `vrad` mode name and `luxelDensity` round trip, `compare` not saved.
- `tests/render_pose_compare.py` (ctest `apex_render_pose_compare`, skips without APEX_P2CE_DIR): compare, wide
  split and density images differ from plain/lit and are byte-identical across runs.
- Example images (scratchpad): cmp2/p1-compare.png (baked | vrad), den2/p1-baked-density.png.
- Density checker is bilinearly filtered by the lightmap sampler, so cells blur at distance; hue is still readable.
