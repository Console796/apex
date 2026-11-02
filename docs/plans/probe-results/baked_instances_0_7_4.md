# Baked mode with instances (0.7.4, slice 6b part 2)

Status: implemented and verified (not committed).

## Investigation: bake side vs viewport

Bake side (`editor::expandBakeInstances`, used by `BakeController`, `render_pose`, `apex_bake_probe`):

- Runs `document::collapseInstance` per visible `func_instance` on an owned `Document::snapshot()`.
  The collapse emits a VMF fragment with fresh sequential object and side ids (restarting at 1 per
  call), moves each brush by the instance's entity pose (rotation plus offset; nested instances use
  the pose already composed into the parent's frame), then `Transaction::create` re-allocates unique
  document ids. The `func_instance` entity is destroyed.
- The baker therefore sees ordinary world and func_detail solids at world positions. A baked face is
  identified by (expanded NodeId, expanded SideId); `LightmapAtlas::fragments` is keyed that way.
  Nothing in the snapshot said which template solid an expanded solid came from.

Viewport side (`RenderMirror` + `editor::InstanceExpansion`):

- The mirror never expands. `setInstanceContents` takes a flat list (nested instances are separate
  entries) of `InstanceContent`: owner NodeId, composed transform, and a fixed-key copy (`keys`) of
  the template `Document`. One child `RenderMirror` per `keys` document is shared by all placements
  of that file; its geometry is template-local and each placement is drawn by adding a preview
  matrix (`content.transform`) to the child's draws.
- Child solids are identified by the template document's own NodeIds and VMF side ids, which have no
  relation to the bake's expanded ids.

So the two sides disagree on identity (NodeId/SideId), on space (template-local vs world) and on
sharing (one mirror per template vs one baked lightmap region per placement). That is why instance
brushes had UV (0,0) and drew black.

## Design

- Identity: `InstancePlacement{owner NodeId of the top-level func_instance, VMF ids of the nested
  func_instances above the template}`. The editor expansion and the collapse both produce it.
  Template solid and side are identified by their VMF ids in the template file.
- Provenance: `collapseInstance` records, per fragment solid, the nested chain, the template solid id,
  fragment-side -> template-side ids, and the transform it applied. `expandBakeInstances` maps
  these onto the created document nodes and returns `InstanceSolidOrigin` rows
  (placement, template solid, expanded NodeId, side map, transform).
- `LightmapAtlas::bindInstances` indexes them: `instances[placement][templateSolid]`.
- `RenderMirror`: a placement whose contents have baked faces gets its own child mirror
  (key = template doc + placement) instead of the shared one, and that mirror meshes its solids
  with `meshSolid(..., LightmapPlacement)`. Two placements of one file therefore get distinct UVs
  and lightmap rectangles; unbaked placements still share.
- `meshSolid` for a placed solid looks the face up by the template side id, checks that the baked
  fragments lie on the face moved by the bake's transform and cover its area (the baker charted the
  rotated plane, so a chart-for-chart comparison is impossible), then emits the baked fragment
  polygons mapped back to template-local space (inverse of the bake's transform) with UVs computed
  from the baked world positions. The draw-time matrix is the instance's current transform, so a
  moved instance keeps showing its (stale) lightmap until the next bake, like a moved world brush.
- Radiance retention across restarts: expansion ids are renumbered by edits, so
  `retainRadiance` maps instance faces through (placement, template solid, template side).

## What the overlay (slice 6a) does

Tool textures are drawn at a fixed brightness in Lit and Baked (`toolFixedBrightness`); that path
works per-draw and so applies to instance contents too, unchanged by this slice.

## Unmapped geometry (still the same)

- func_detail: baked and mapped, in the map and inside instances (the bake scope is worldspawn plus
  func_detail; the mirror looks the face up by id like any other solid).
- func_brush and every other brush entity, in the map or inside an instance: not baked, so no
  fragment; they stay black. For a placed solid with no baked side `meshSolid` drops the lightmap
  binding altogether. The HUD line now reads "Other brush entities: black; props/displacements: Lit".
- Props and displacement sides: Lit shading, as before.
- Placements have no reflection or scale (the entity pose is rotation plus offset only).
- A moved or re-rotated instance keeps its baked lightmap (positioned by the new transform) until the
  next bake finishes; a face whose baked fragments no longer lie on the moved face is left unmapped.

## Test poses and verification

test1.vmf (instances: three func_instance, origins (-1024,0,-144) x2 and (0,0,0); 5 placements with
nesting; `bake instances placements=5 baked-solids=140`). Pose format `x y z pitch yaw roll`, 960x540,
`--mode baked --bake-passes 4 --bake-bounces 1 --bake-threads 3 --bake-scale 0.78`:

- A: -1024 -400 -60 0 90 0
- B: -700 0 -60 0 180 0
- C: -1300 0 -60 0 0 0

Images (scratchpad): `inst1` (mirror mode, new), `inst1exp` (`--instances expanded`, reference),
`inst1lit` (Lit), `inst1unbound` (before this change). All under
`/tmp/claude-1000/-mnt-hdd-projects-apex/535ad622-6121-4838-a225-ecc2cfa4a9c0/scratchpad/`.

| Pose | mirror vs expanded (max lum diff / mean abs diff) | Baked/Lit mean luminance | old (unbound) vs expanded mean |
|---|---|---:|---:|
| A | 0.0007 / 0 | 0.987 | about 5-16% darker |
| B | 0.0007 / 0 | 1.009 | across the three poses |
| C | 0.0007 / 0 | 1.008 | |

Instance faces are lit, not black, and match the pre-expanded document render to within one 8-bit step.

Set 2 poses 11/21/31/41 (README coordinates; `--bake-passes 4 --bake-bounces 1 --bake-scale 0.78`):
mirror and expanded modes are pixel-identical (mean abs diff 0). The set 2 views do not look at any
instance content (instances are at x=-1024..0, y=0; the poses look away from them at y of -1135..-693),
so the set 2 parity numbers in `baked_render_set2_0_7_4.md` are unaffected and there is no instance
comparison against the in-game captures to make. One side effect worth knowing: in mirror mode the
renderer requests the unexpanded templates' materials too (materials=74, failed-materials=6 versus
62/0 for expanded); this is the existing child-mirror material request, not new.

Tests (`tests/render/instance_lightmap_test.cpp`, synthetic map: one 64x32x16 box template placed
twice, once at yaw 90 with offset, under a point light): (a) a rotated placement's faces map to the
atlas slots of their expanded faces and sit on the baked loop after the transform; (b) two placements
of one file get disjoint fragments and different UVs; (c) a `BakeSession` over the expanded scene
publishes lit faces for both placements.
