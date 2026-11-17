# 0.8.9 P0: what a structural brush costs vis

Plan 0.8, sub-milestone 0.8.9 (D16). The plan asked for the small/thin structural thresholds to be
confirmed against `.prt` portal counts on the corpus. That cannot be done on the corpus, and the numbers
below come from a synthetic probe instead.

## The corpus has no portal files

All 510 VMFs under `$APEX_P2CE_DIR/sdk_content/maps` are Puzzle Maker instances. None seals, so the native
P2CE vbsp writes no `.prt` for any of them (503 exit with code 1, 7 compile with no world faces). None has a
displacement either (`dispinfo` occurs in 0 files).

## The probe

`tools/structural_probe.py`, 2026-11-15, native `bin/linux64/vbsp` (Apr 23 2026). One sealed 1024×1024×256
room, plus one extra brush, compiled twice: once with the brush in the world and once with it in a
`func_detail`. The `.prt` header gives clusters and portals. The empty room is 4 clusters and 4 portals,
because vbsp's 1024-unit block grid splits it.

| Brush | Structural clusters / portals | As func_detail | Portals added |
| --- | --- | --- | --- |
| floor cube 4…128 (each) | 12 / 20 | 4 / 4 | 16 |
| floor cube 256 (reaches the ceiling) | 8 / 8 | 4 / 4 | 4 |
| floating cube 8…64 (each) | 16 / 32 | 4 / 4 | 28 |
| floor-to-ceiling pillar 4…128 wide (each) | 8 / 8 | 4 / 4 | 4 |
| half-room wall, 1…32 thick (each) | 4 / 3 | 4 / 4 | −1 |
| floor slab 512×512, 1…16 thick (each) | 12 / 20 | 4 / 4 | 16 |
| wall trim 512×16, 8…64 high (each) | 8 / 13 | 4 / 4 | 9 |

## What it shows

- **Size does not change the cost.** A 4-unit cube adds exactly the portals a 128-unit cube does. The cost
  depends on how many room boundaries the brush touches: free-floating costs most, a pillar from floor to
  ceiling least. Thickness doesn't matter either: a 1-unit slab costs what a 16-unit one does.
- **Size changes the benefit.** A brush pays for its portals only by hiding things behind it. A half wall
  of any thickness divides the room and *removes* a portal. A floor cube, slab or trim hides nothing.
- `%compiledetail` (the material flag that makes a world brush detail) is set by one material in the whole
  install, `tools/toolspropcombine`, so the Structural view treats world brushes as structural without
  reading it.

## The rule Apex uses

The check flags a structural brush when it is too small to hide a player from any direction. The test:
along no axis are both perpendicular extents of its bounds at least **64 units**. 64 is just under the
72-unit standing player height, so a brush that fails it can't hide a standing player from any side.

| Probe brush | Flagged |
| --- | --- |
| floor cube 4…32, pillar 4…32, trim 512×16×*, floating cube 8…32 | yes |
| floor cube 64…256, floating cube 64, pillar 64+ | no |
| half wall 1…32 (512×256 face) | no |
| floor slab 512×512×1…16 (512×512 face, looking down) | no |

The probe doesn't settle where the 64 sits; that's a judgment on occlusion, not on portal cost. Three
cases are never flagged: brushes with a `tools/*` face other than nodraw (hint, skip, clip, trigger…),
displacement brushes (VBSP does not let displacements cut vis), and brushes in an entity. A floor slab escapes the rule because it can be a real storey floor; the rule can't
tell that apart from a decorative slab resting on one.
