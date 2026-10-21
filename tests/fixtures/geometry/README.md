# Geometry corpus findings (0.1.6)

The proprietary VMFs stay in `APEX_P2CE_DIR/sdk_content/maps`. The Apex-authored
`expected_invalid.txt` records exact map/solid/diagnostic signatures, including data
inconsistencies in otherwise buildable brushes. A new, vanished, or changed signature fails
the test. There is no automatic acceptance/update switch.

## Corpus observations

On the 512-file P2CE corpus available on 2026-10-20:

- All 11,698 solids build from planes.
- 10,948 also pass D8 point-data reconciliation; 750 fail strict validation.
- 752 solids have inventoried exceptions across 12 VMFs. The extra two are a redundant
  source side and a point-data disagreement in a solid whose stale points fall back to planes.
- All 9,207 point-data faces are compared with plane intersections, including those in solids
  rejected by reconciliation. Of the 750 rejected solids, 730 also have a point-data vertex-set
  mismatch. The remaining 20 fail strict planarity, convexity, or closure checks despite vertex
  agreement within the looser trust distance.
- Every plane-buildable solid is rebuilt through actual vertex triangles and checked for
  equal vertex sets and matched face identity/texture fields. When reconciliation succeeds,
  its authoritative geometry is used; otherwise the plane-built geometry is used. The redundant
  input side is inventoried because a side without a boundary face cannot retain a face identity.

| Map (relative to `sdk_content/maps`) | Reconciliation rejects | Exceptions |
| --- | ---: | ---: |
| instances/community/elevators/base/elevator_arrival_clean_base.vmf | 169 | 169 |
| instances/community/elevators/base/elevator_arrival_dirty_base.vmf | 168 | 168 |
| instances/community/elevators/base/elevator_departure_clean_base.vmf | 144 | 144 |
| instances/community/elevators/base/elevator_departure_dirty_base.vmf | 167 | 167 |
| instances/cubedroppers/dropper_clean.vmf | 1 | 1 |
| instances/cubedroppers/dropper_over.vmf | 1 | 1 |
| instances/door/door_bipart.vmf | 7 | 7 |
| instances/door/door_entities/door_ents_noareaportal.vmf | 4 | 4 |
| instances/door/portal_door_entities/portal_door_frame_white_dirty.vmf | 0 | 1 |
| instances/labs/observation_room_256x128_1b.vmf | 23 | 24 |
| instances/labs/observation_room_64x128_1.vmf | 22 | 22 |
| test1.vmf | 44 | 44 |

These are **kernel/data compatibility failures, not demonstrated Hammer-invalid brushes**.
Plane intersections produce closed, convex, planar solids for every entry. Independently rounded
or stale point-data coordinates can exceed the kernel's `1e-7` welding and `1e-6` planarity
thresholds while still meeting the per-side `1e-3` plane trust rule. Mixing trusted loops with
plane-derived fallback loops can leave unmatched edges. Do not fix this by silently increasing
tolerances or discarding trusted data in the kernel.

For example, `test1.vmf` solid 1196 has maximum point/plane-vertex drift about 0.000269493
units but face nonplanarity about 0.000261284 units: it passes trust and fails planarity.
The maximum observed point-to-nearest-plane-vertex distance is 0.0625 units (departure dirty
base, solid 49059); maximum point-data face nonplanarity is about 0.00628682 units.
Observation-room solid 34 differs by 0.003 units and is successfully rebuilt using the specified
per-side fallback. Portal-door-frame solid 1805 has six input sides but five boundary faces.

Strata Hammer visual inspection was not performed in this Linux sandbox. A human should confirm
these imported brushes' appearance and decide how D8 should handle serialized point-data drift
without weakening D9's geometry invariants. Existing case-5 fixtures and the one-new-face clip
inheritance fixture already cover the requested synthetic cases and are retained unchanged.

## Native VBSP experiment

`python3 tools/vbsp_equivalence.py` builds isolated original/re-planed copies of `test1.vmf`
using `bin/linux64/vbsp -game <p2ce> -instancepath <sdk_content/maps>`, then reads both with
`vbspinfo`. `--output-dir <new-directory>` retains the artifacts. This manual gate is deliberately
outside `ci.py`. The tool re-planes all 119 solids / 722 sides from plane intersections using
maximum-precision vertex coordinates. It leaves point data and every other byte unchanged;
the script checks that byte-preservation condition before compiling. Using plane intersections
isolates D8's plane-to-vertex-to-plane premise even where trusted point data fails validation.

Native tools reported build date Apr 23 2026. Both compiles succeeded:

| Count | Original | Re-planed |
| --- | ---: | ---: |
| brushes | 362 | 362 |
| brush sides | 3,399 | 3,403 |
| planes | 3,500 | 3,522 |
| faces | 453 | 453 |
| original faces | 429 | 429 |
| vertices | 1,255 | 1,255 |
| triangles | 1,165 | 1,165 |

All other VBSPInfo object counts and lump byte sizes match. Plane bytes change from 70,000
to 70,440; brush-side bytes from 54,384 to 54,448. Reading the v25 BSP brush-side bevel flags
shows 1,197 versus 1,201 bevel sides; non-bevel side counts match. Eight brush records change
side count (six gain one bevel, two lose one). Thus the changed brush-side count comes from
VBSP collision bevel generation, which is sensitive to the changed floating-point plane
representation. This does not establish collision equivalence: identical face/triangle counts
alone are insufficient. The script correctly exits 1 for these differences. D8 requires owner
review of collision consequences before treating case 4 / sub-milestone 0.1.6 as accepted.
