# 0.7.6 step 2: nested cascades, probe relocation, normal-offset bias

Measured on the RX 6600 (RADV, Vulkan 1.4, ray query) with validation on. These are not
llvmpipe numbers and not production performance numbers: the update path is still the
synchronous `waitIdle` design from step 1. The option stays default off; with it off no
probe code runs and the only ABI change is zeroed trailing frame-block bytes.

## What was built

- **Coarse cascade.** `ProbeSettings::cascades` (default true while realtime indirect is on)
  adds a second grid at `cascadeScale` (4x) the spacing with `max(dims/2, 2)` probes per axis,
  so it covers twice the fine extent. It has its own double-buffered maps, cursor and
  refresh phase, and is camera-centred and snapped to its own spacing. `probesPerFrame`
  budgets the fine cascade; the coarse cascade gets the same fraction of its probes, so both
  finish a refresh in the same number of frames.
- **Blend.** `cascadeIrradiance` (probes.inc) weights the fine result by its distance to the
  fine grid edge, ramping over `cascadeFade` (1) fine cells, and fills the rest from the
  coarse cascade. The coarse result fades to zero over one coarse cell at its own edge, so
  beyond the coarse cascade the result is still dark. When the fine weight is 1 the coarse
  cascade is not sampled.
- **Transport.** The update shader's bounce lookup at ray hits uses the same two-cascade
  sampler on both previous-frame grids (a shared `CascadeContext` buffer), so far hits feed
  the probe rather than going dark.
- **Relocation.** Per probe, lane 0 casts six axis rays from the grid point. If any hits a
  back face (probe inside a solid) the nearest exit is chosen, ties by fixed axis order, and
  the probe moves out along that face's normal by `exit distance + relocateRadius*spacing`.
  Otherwise a front face closer than `relocateRadius` (0.25 spacing) pushes it away. The move
  is capped at `relocateMax` (0.5 spacing). The offset is stored (as a fraction of spacing) in
  the unused `depth` channels of texels 0/1 and applied by the sampler. A probe that is still
  mostly back faces after the move is marked invalid and excluded as before.
- **Normal-offset bias.** Step 1's 2-unit normal offset is kept and scaled by `cascadeScale`
  for the coarse cascade. The sampler now also multiplies each probe weight by a squared
  surface-side term, `(0.5 + 0.5 * dot(n, toProbe))^2`, so probes behind the shading plane
  contribute little before the Chebyshev test.
- **ABI.** `FrameBlock` grows append-only from 496 to 560 bytes (coarse address, dims, origin,
  params); `indirectParams.z` now carries the fade width. `static_assert`s and `frame.inc`
  updated. No changes in `src/ui` or `src/viewport`.

## Fixes made to the unreviewed WIP

- The coarse dimensions were derived from the previous settings, so the first update reset
  twice and the second recreated the coarse maps. That made the first run of an
  `accumulate` differ from a rerun; the "deterministic repeat" test failed.
- The transport lookup used the dispatched cascade's own previous map as the fine grid, and
  the fine address as the coarse one.
- Relocation took the first axis with an exit rather than the nearest.
- Both cascades had the full budget; the history-budget test needed `cascades = false`
  (the ceiling sits inside the fine grid's last cell, so the coarse cascade contributes).
- Debug and bisect code was removed from the tests.

## Measurements (8x8x6 / 6x6x6 grids, 64-unit spacing, 16 frames, albedo 0.65)

| Check | Result |
| --- | --- |
| Ceiling mean luminance, probes on | 0.0585 (step 1: 0.0422; the coarse cascade and side weighting add light) |
| Thin-wall dark/bright, wall phase 0 / 16 / 32 | 0.33% / 0.27% / 0.45% (threshold 5%; step 1 was 0.15% / 0.049% / 0.0036%) |
| Cascade sweep, ceiling x = -240..-40, fine grid edge at -160 | -60: 0.0596; -160: 0.0211; -190 (outside fine): 0.00126; cascades off at -190: exactly 0 |
| Largest adjacent 5-unit step in the sweep | 0.0056, 9% of the sweep maximum (test tolerance 15%) |
| Relocation, probe at the centre of the 16-unit wall | offset -0.375 spacing (24 units); sampled at x=8, 16 units outside the wall at x=24; invalid (-1) without relocation, valid with |
| Repeat/reset determinism, budgeted history, sky injection, rebasing | pass |
| Validation errors / warnings | 0 / 0 |

## Limitations

- The seam tolerance is loose. The field falls roughly 50x across the sweep, so a 15%
  step limit proves there is no hard fine/coarse switch, not that the two cascades agree.
  I did not measure fine and coarse against each other away from the edge.
- Thin-wall leakage is about 2 to 100 times higher than step 1 (still under 0.5%, far below
  the 5% threshold). The coarse cascade at 256-unit spacing cannot resolve a 16-unit wall at all;
  leakage there relies on the Chebyshev test, the bias and the surface-side weight, and is only
  exercised in the test where the fine cascade fully covers the walls.
- Relocation is one-shot per update, along an axis and capped at half a spacing. Thick solids
  stay invalid. It is not verified that the destination is free of other geometry. Offsets
  are stored in texel channels and blended with no history smoothing.
- Outside the coarse cascade the result is still dark, and any grid shift resets history
  for both cascades.
- Coarse and fine updates are serial dispatches; hardware cost was not measured.
- Only the Lit view with the option on is covered; the editor and viewport UI are untouched.

## Static lights inject (decision, 2026-11-02)

Step 1 injected only lights with `_indirectmode 2` (Dynamic). Every light defaults to 1 (Static), so in
ordinary maps the probe volume received nothing and the overlay showed no change. The preview exists to show
what the bake will show, and VRAD bounces Static lights, so modes 1 and 2 now both inject, for local lights
and for the `light_environment` sky emitter. Only None (0) stays out. There is no separate option.

Evidence: the probe test now checks that None gives exactly zero and Static is byte-identical to Dynamic.
`test1.vmf` (all `light_rt` Static, 17 lights), pose p1, 640x360, 128 accumulation frames: mean luma
46.0 off, 56.5 with probes; 94.6% of pixels differ by more than 2 levels. Before this change the same
run gave no injection for these lights. `render:*,renderer:*,lighting*`: 80 cases, 97,098 assertions pass.
Not compared against VRAD; the `vrad -final` ratio on this scene is still owed.
