# 0.8.6 P0: overlays in the corpus

Plan 0.8, sub-milestone 0.8.6 (D10–D12). Counts from `tools/overlay_corpus_counts.py` over every
`.vmf` under `$APEX_P2CE_DIR/sdk_content/maps`, 2026-11-12. Entities inside `hidden { }` wrappers
are counted.

## Counts

| What | Count |
| --- | --- |
| VMFs scanned | 510 (the plan says 512) |
| Maps with an `info_overlay` | 25, all instances |
| `info_overlay` entities | 78 (28 inside `hidden` wrappers) |
| Overlays with an empty `sides` | 20 |
| Overlays on a displacement | 0 (the corpus has no displacement sides at all under overlays) |
| `info_overlay_transition` | 0 |
| `infodecal` | 3, in 2 maps |
| Listed side IDs that do not exist in the map | 0 |
| Distinct overlay materials | 25 |
| `env_cubemap` with a non-empty `sides` | 0 |

Faces per overlay (`sides` length): 0: 20, 1: 28, 2: 5, 3: 5, 4: 3, 5: 4, 6: 6, 7: 3, 8: 2,
24: 2. The maximum is 24 (`coop/coop_endlevel_room.vmf`, `p2editor/coop_exit.vmf`).

Empty side lists: `coop/coop_spawn_room.vmf` (6), `coop/coop_spawn_room_paint.vmf` (6),
`coop/coop_spawn_room_bridge.vmf` (6), `community/elevators/base/elevator_departure_dirty_base.vmf`
(1), `community/observation_rooms/observation_room_192x128_1.vmf` (1). D11 rule 4 keeps these and
flags them; they are already in the shipped content, so the flag is a warning, not an error.

Decals: `community/observation_rooms/observation_room_256x128_1.vmf` (2) and
`p2editor/faith_plate_target.vmf` (1).

Consequences for 0.8.6:

- The displacement clip path and `info_overlay_transition` have no corpus coverage; their tests
  are synthetic.
- The BSP comparison runs on the 20 maps whose overlays list faces.

## Who clips: the engine, not VBSP

D10 says Apex clips "as VBSP does". VBSP does not clip overlays. It resolves each side ID to the
compiled BSP faces made from that side and writes the overlay's basis, quad and face list to the
OVERLAYS lump; the engine clips the quad against those faces when the map loads
(`COverlayMgr::Surf_CreateOverlayFragments`). Apex clips against the brush faces instead of the
compiled faces, which differ where CSG or a split removes part of a face. So the comparison test
checks the lump's quad, basis and texture range exactly and checks the fragment area only as a
lower bound (Apex area ≥ the area that lands on the compiled faces).

## The P2CE OVERLAYS lump (lump 45, version 3)

Decoded from a vbsp compile of `instances/gameplay/faithplate_black.vmf` (BSP v25, FACES lump
version 2):

```
int32 numOverlays
int32 numFaceRefs
int32 faceRefs[numFaceRefs]           // BSP face indices, all overlays' lists back to back
struct (108 bytes) [numOverlays] {
  int32 id                            // the entity's index among overlays
  int32 texinfo
  int32 unknown0                      // 0 in every compile seen
  int32 firstFaceRef
  int32 faceCount
  float U[2]                          // StartU, EndU
  float V[2]                          // StartV, EndV
  float uvPoints[4][2]                // uv0..uv3 (x, y)
  float basisU[3]
  float origin[3]
  float normal[3]
  int32 unknown1                      // -1 in every compile seen
}
```

BasisV is not stored. Presumably the engine rebuilds it as `normal × basisU` (Source 2013 keeps a
flip flag in the third component of a uv point, which this layout no longer has). A mirrored
overlay (BasisV = `basisU × normal`) would need a flag somewhere, and `unknown1` is the likely
place. No corpus overlay is mirrored, so this is unconfirmed; a mirrored probe map would settle it.

## Oracle results (tests/document/overlay_oracle_test.cpp)

`tools/overlay_oracle.py` compiled the 21 corpus maps that have an overlay with a non-empty
`sides` with the native vbsp (`-instancepath` set to the corpus root). Three failed to compile
because a nested instance is missing from the install (`elevator_arrival_dirty_base`,
`coop_spawn_room_bridge` and `two_player_airlock_underground`). They are recorded as `skip` lines
in the fixture. The 18 that compiled give:

- **43 overlays matched.** All 43 visible listed overlays match their lump entry exactly: quad,
  BasisU, BasisNormal and texture range. The rebuilt BasisV (N × U) equals the VMF's.
- **10 hidden overlays are not compiled.** VBSP leaves entities inside `hidden { }` wrappers out
  of the compile. Both `destructed_*_soundscape_override_alt` maps compile with no overlays at all.
- **4 overlays compile with no faces.** In `bts_wall_module_512x1024_01a`, the listed sides
  produce no compiled face in the sealed probe map. VBSP still keeps the overlay.
- **3 overlays sit on back-facing faces.** In `shaft_vista_v2`, `observation_room_256x128_1b` and
  `departure_elevator_column_blocker`, the listed face's normal is opposite to BasisNormal. VBSP
  keeps these overlays and the engine draws them. Apex used to reject faces facing away; it now
  clips onto them, with the winding flipped to match the face.
- **Area check.** The worst shortfall of Apex's clipped area against the area the quad covers on
  the compiled faces is 1.8e-4 of the area, from BSP float vertices.
