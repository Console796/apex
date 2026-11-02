# 0.7.5 slice A: VRAD round trip as a lightmap source

Status: round trip, BSP reader, mapping, atlas and `render_pose --mode vrad` are done and tested. The
interactive UI selector (DisplayMode::Vrad, controller, menu, HUD) is NOT wired yet.

## Design

- Pipeline: snapshot the document, expand instances on the copy (`editor::expandBakeInstances`), write a temp
  VMF into a fresh dir under the system temp dir, run `compile::CompileJob` (vbsp, optional `vvis -fast`, vrad)
  through the normal runner and tool discovery. Nothing is written to the game dir or the user's maps.
  One run in flight; cancel kills it; an edit marks the result stale (still shown, labelled stale).
- Reading: bsppp, HDR lumps 53 (lighting) and 58 (faces). ColorRGBExp32 decoded as (m+0.5)*2^e/255, style 0
  only, luxel grid (extents+1) per axis.
- Lattice: the exact VRAD lattice is kept. Each BSP face becomes a `BakeFace` chart from the texinfo lightmap
  vectors, stored mins and size (`lighting::bspChart`); no resampling. `LightmapAtlas::compiled = true` lets
  the mesher keep fragments that lie on a side and cover no more than it.

## Findings that deviate from the brief

- P2CE vbsp writes no FACEIDS lump, so `hammerfaceid` is unavailable. Mapping is geometric: matching plane
  (normal dot >= 0.9999, distance within 0.25), each convex BSP face clipped to each side polygon (pieces under
  area 0.5 dropped). Where coplanar solids overlap, one fragment feeds several sides (`shared` stat), rather
  than "ambiguous stays unmapped".
- `dface_t.side` orients the face against its BSP node, not the light. The face plane is already the lit one;
  flipping on `side` was a bug (caught by the synthetic room, faces went unowned).
- BSP loops are clockwise, Apex loops counter-clockwise; the loop is reversed.

## Default vrad flags

`-noextra`, a setting (`vrad/flags`). light_parity measured with `-bounce 0 -noextra -aoscale 0`; bounces are
left on by default here since this is meant to show the real compile, `-noextra` keeps the round trip quick.

## Results

- Synthetic lit room (256 cube, one `light`), real tools, 6/6 fragments mapped, 0 unowned, ~6 s with vvis.
  Test: `tests/render/vrad_real_tools_test.cpp` (skips without APEX_P2CE_DIR).
- Display scale: default VRAD scale 1.0 (Baked stays 0.62). VRAD/Apex-baked luminance ratio on the room was
  1.03 at light brightness 2000 (poses 1 and 2), but 1.91 at brightness 200 with `--bake-passes 16
  --bake-bounces 0`. So parity is not a single constant; not enough data to fit a scale.
- test1.vmf: its lights are light_rt/light_rt_spot, which VRAD ignores (the game renders them dynamically).
  VRAD sees only 21 texlight emitters (mean radiance about 0.0076), so renders are near black and ratios to
  set 2 captures are 0.01-0.12. The scale cannot be fitted on test1.
- Images (scratchpad): vrad/f1/*-vrad.png (test1), vrad/baked062, vrad/room_vrad, vrad/room_baked,
  vrad/room_vrad2, vrad/room_baked2.
