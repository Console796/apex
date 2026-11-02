# Baked mode: lightmaps for brush entities (0.7.4, slice 8)

Status: implemented and verified (not committed).

## Which brush entities get lightmaps, and which cast

VBSP compiles every brush entity to its own BSP model; each non-tool face of a model gets a lightmap, and
VRAD lights it at the position it was authored in (an entity that moves at runtime keeps that lightmap).
`func_detail` is merged into the world model and is lit and casts. The other brush entities (func_brush,
func_wall, func_door, func_movelinear, func_rotating, func_breakable, func_illusionary, ...) are lit by
their own lightmaps but are not in the tree VRAD traces rays through, so they receive light and do not
cast. This is already the rule in D13 (0.6) and in `bakeCastersFromDocument` / `RenderMirror::brushCasts`
(world and func_detail only); nothing was wrong, so the caster rules are unchanged.

Faces VBSP gives no lightmap: tool materials (`tools/*`, which `material()` marks `noLight`), `nolight`
materials, sky/nodraw, and displacement sides (baked in 0.8). Brush entities that compile to volumes or
portals (`trigger_*`, func_areaportal(window), func_occluder, func_viscluster, func_clip_vphysics,
func_precipitation) are excluded by class even when a mapper put a drawn material on them.

## What was missing

`bakeSceneFromDocument` already charted every solid, but `setupBake` erased the faces of every solid outside
worldspawn/func_detail. The viewport needed no change: `meshSolid` looks a face up in `LightmapAtlas::fragments`
by (solid NodeId, side id), and `RenderMirror` draws any baked solid with that lookup, so a brush entity with
baked fragments is mapped like a world brush. The key is the solid's own NodeId (the entity's NodeId is not
needed), stable across edits, so `faceKeys` and `diffScenes` work unchanged. Brush entities inside
func_instance contents were already covered by the instance path (they are expanded into the document as
entities), and now bake like any other.

## Change

- `src/render/bake_setup.cpp`: bake scope is every visible brush solid except those of unlit entity classes
  (`lightmapped`). Casters unchanged.
- `src/ui/gpu_host.cpp`: HUD line now says only "Props/displacements: Lit (not baked)".
- Tests (`tests/render/lightmap_atlas_test.cpp`): lightmapped entity classes get faces and atlas slots, tool
  volumes and nolight faces do not; face keys of a brush-entity solid are unchanged by an unrelated edit;
  a brush entity is lit but does not occlude.

## Render check

test1.vmf has no brush entities other than func_detail, so the check uses a scratch copy
(`scratchpad/test1_be.vmf`: test1 plus a func_brush, func_illusionary and func_door, one 64x64x64 box each,
material PBR/TILES/DIRT/WHITE_TILE_64_DIRT_02 at x -1100..-1036, z -130..-66, y 100 / -40 / -160 centred
around the room's pedestal). `apex_render_pose --mode baked --bake-passes 4 --bake-bounces 1 --bake-threads 3
--bake-scale 0.78`, pose `A -1300 0 -60 0 0 0`, 960x540. Mean luminance of the three box regions (left to
right, 0-1 gray):

| | func_illusionary | func_brush | func_door |
|---|---:|---:|---:|
| Baked before | 0.0076 | 0.0001 | 0.0056 |
| Baked after | 0.047 | 0.0149 | 0.042 |
| Lit | 0.033 | 0.0038 | 0.029 |

Images: `be_before/A-baked.png`, `be_baked/A-baked.png`, `be_lit/A.png` in the session scratchpad
(`/tmp/claude-1000/-mnt-hdd-projects-apex/535ad622-6121-4838-a225-ecc2cfa4a9c0/scratchpad/`). Baked is somewhat
brighter than Lit on the scratch boxes (Baked adds bounce light; Lit has none).

## Still unmapped

Props and displacements (Lit shading in Baked). Entity faces are lit but not casters, so a func_brush neither
shadows nor bounces light for the world in the bake, matching VRAD.
