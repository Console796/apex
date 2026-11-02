# Incremental invalidation for the live Baked view (0.7.4, slice 7)

Status: implemented and verified (not committed). Plan: docs/plans/0.7.md D4.

## Before

Every document change called `BakeController::invalidate()`, which dropped the session; after 250 ms the
worker rebuilt the whole scene and restarted every face from zero passes. The previous radiance was only
kept as a visual placeholder (`LightmapAtlas::retainRadiance`, `retainStale`).

## Design

Three pieces, bottom to top.

1. `lighting/bake_diff.{hpp,cpp}`: `diffScenes(before, after)` -> `BakeDiff` (per new face: matching old
   face or -1, `dirty`, `bounceDirty`; or `full` with a reason).
2. `BakeSession`: per-face, per-level pass counters (`facePasses`), `takeState()` (moves the converged
   per-face sums/counts and the scene's faces/lights/materials out), and a `restart(..., state, diff)` overload.
   Clean faces keep sums and counts and are skipped by the scheduler; dirty faces restart from zero.
3. `BakeController`: keeps the retired session as `previous_` across `invalidate()`, the rebuild job computes
   the diff and builds the new session from the previous state.

### Face identity

`FaceKey{solidId, sideId, fragment ordinal within that side}`. For instance-expanded faces the controller
supplies keys built from the atlas `templateOfSide` map (placement + template solid/side), because expansion
renumbers NodeId/SideId on each edit. A face is "unchanged" when its key matches and a hash of vertices,
normal and chart (mins, dimensions, axes, offset, origin) and of material name + albedo are equal.

### Dirty rules (direct lighting restarts)

* New face, changed geometry/chart, changed material name or albedo: dirty.
* Light added/removed/changed (matched by `Light::id`, every field compared; lights without direct
  contribution ignored): both the old and the new state dirty the faces they can reach:
  * point: faces whose bounding sphere is within `effectiveRange(light, kLightThreshold)` of the origin
    and not entirely behind the light plane;
  * spot: additionally inside the outer cone (bounding sphere vs cone);
  * sun: faces with `dot(normal, -direction) > 0`;
  * ambient (sky): unbounded, full restart.
* Changed geometry (a changed/added/removed face, old and new bounds) is an occluder sphere `(c, r)`. A new face
  is dirty when it is
  * within `r + kGeometryMinRadius or kGeometryScale * r` of the occluder (contact shadows, sky visibility,
    near-field bounce), or
  * in the shadow cone of a direct point/spot light that can reach it (bounding sphere vs cone from the light
    through the occluder sphere, beyond the occluder), or
  * in the shadow cylinder of the sun (line from the face toward the sun passes the occluder sphere).
  This is exact up to bounding spheres for direct lights (conservative), and a stated tolerance for sky.
* Material change to or from a sky tool material, an ambient light change, no usable overlap with the previous
  scene (fewer than half the new faces matched), or changed baker settings (seed, rays, bounces): full restart.

### Bounce coupling

Dirty faces' new direct light changes the bounce other faces receive. Faces whose bounding sphere is within
`kBounceRadius` of a dirty face are `bounceDirty`: they keep their converged direct sums but restart every
bounce level from zero. Everything else keeps all levels.

### Approximations (documented, not exact)

* Light range uses `kLightThreshold` (1/256 of linear HDR white): luxels below it are treated as unlit.
* Sky/ambient and bounce influence are cut off at fixed radii. The sky visibility change from an occluder of
  bounding radius r seen at distance d is at most about r^2 / (2 d^2) of the hemisphere; the 8r bound keeps the
  ignored remainder under ~0.8 %. Bounce beyond `kBounceRadius` of a changed face is ignored.
* To bound drift, the controller forces a full restart after `kMaxIncrementalRestarts` consecutive incremental ones.
* Clean faces keep their converged samples; continued refinement of a clean face whose index shifted uses seeds
  of its new index (statistically equivalent, not bit identical). Dirty faces equal a full bake at the same
  pass count when indices are stable.
* A bounce-dirty face briefly shows direct only for bounce levels not yet resampled.

## Controller wiring

`BakeController::invalidate()` moves the session to `previous_`; the rebuild job (worker) calls `takeState()` on it
after the last cancellation check, builds the new session and calls the incremental `restart` with keys from
`LightmapAtlas::faceKeys()` (template identity for instance faces). If a job is cancelled after `takeState`, the state
is lost and the next job does a full restart. A failed job keeps `previous_`. Display radiance still transfers through
`retainRadiance`. After `kMaxIncrementalRestarts` (16) consecutive incremental restarts the next one is full.
Status text is unchanged.

## Tests

* tests/lighting/bake_diff_test.cpp: classification (light moved/added/removed, range, facing, spot cone, sun,
  moved brush, shadow cone, added/removed faces, albedo, fragments, full fallbacks).
* tests/lighting/bake_session_test.cpp: bounces=0 incremental result bit-equal to a full bake of the new scene and
  clean faces untouched (buffers and pass counts), nothing-changed restart, interrupted-session resume, camera
  order among dirty faces, full fallbacks, republish after takeState, bounce tolerance (first-hop faces bit-equal,
  far wall within 2% of the peak; observed 2.6e-5 against 0.31).
* tests/render: `LightmapAtlas::faceKeys` equals `lighting::faceKeys` for plain faces; instance faces follow template
  identity, one scope per placement.
* Full suite: 1040 tests pass offscreen (`ctest -j4`), `check_layers.py` clean.

## Measurement (release, 3 cores, APEX_MEASURE=1 apex_measure_tests)

test1.vmf: 371 faces, 17 lights, bounces=2, maxPasses=16, flat 0.5 albedo (no assets). Initial full bake 10.8 s.
Moving one point light, time to idle (diff+adopt is ~0.4 ms):

| light (reach) | move | dirty faces | bounce-dirty | incremental | full restart |
|---|---|---|---|---|---|
| 0 (1086) | 16 | 174 | 371 | 10.34 s | 10.73 s |
| 0 (1086) | 128 | 181 | 371 | 10.37 s | 10.75 s |
| 11 (348) | 16 | 96 | 371 | 10.22 s | 10.78 s |
| 11 (348) | 128 | 107 | 371 | 10.29 s | 10.97 s |

Speedup is only about 1.04-1.07x. Bounce traversals are ~93 % of the time and the test map is about 1500 units
across, so a 512-unit coupling radius around dirty faces re-runs bounce on every face. Trying radius 256 gave 1.06-1.08x
(still 371 bounce-dirty) and 128 gave 1.03-1.09x (347 bounce-dirty): the dirty set is spread over the whole map, so
shrinking the radius does not help here. Direct-only restarts (bounces=0) skip all clean faces and do scale with the
dirty fraction. A larger map, fewer bounces, or a smarter bounce criterion (e.g. weighting by the size of the direct
change) would be needed for a real win on bounced scenes; left as follow-up.

## 7b: edit-first scheduling

Time-to-idle is the wrong metric on a map where everything is bounce-coupled, so after an incremental restart the
scheduler converges what the edit changed first:

1. Dirty faces alone run direct and bounce level 1, each at K0 and then at the maximum (clean and bounce-only-dirty
   faces keep displaying their old radiance). Level 1 reads only direct radiance, which is final for every face by then,
   so this phase is exact. Levels 2 and up are not in this phase: they would read the still-unrefreshed level 1 of
   bounce-dirty neighbours.
2. The ordinary schedule then runs over all faces, with dirty faces ahead of the rest in every traversal (camera order
   inside each group). This refreshes the bounce-dirty neighbours and the deeper levels.

Consequences: with bounces=0 the result is unchanged (bit-equal to a full bake). The dirty face's own level 1 is bit-equal
to a full bake; a bounce-dirty neighbour's level 1 is drawn after the dirty face's direct has refined, so it agrees only
statistically (test tolerance 2 % of the peak). Publication is unchanged: dirty faces appear in the next tick's
publication as soon as they have samples.

The delta-based bounce criterion (skip bounce refresh where the dirty faces' direct change is negligible) was not
attempted: it needs old and new direct per face and a tolerance that is hard to bound; the 512-unit coupling stays.

Measured on test1.vmf (same setup as above), seconds; "full" is a full restart observed on the same dirty faces:

| light, move | dirty | first visible change (inc / full) | dirty direct+bounce1 converged (inc / full) | dirty all levels converged (inc / full) | map idle (inc / full) |
|---|---|---|---|---|---|
| 0, 16 | 174 | 0.0002 / 0.0032 | 2.78 / 7.32 | 10.62 / 10.92 | 10.80 / 10.92 |
| 0, 128 | 181 | 0.0002 / 0.0028 | 2.70 / 7.05 | 10.03 / 10.72 | 10.20 / 10.72 |
| 11, 16 | 96 | 0.0002 / 0.0026 | 2.29 / 6.99 | 9.84 / 10.61 | 10.03 / 10.64 |
| 11, 128 | 107 | 0.0002 / 0.0027 | 2.49 / 6.88 | 9.91 / 10.48 | 10.08 / 10.51 |

The first visible change is immediate and the dirty faces' direct plus first bounce converge about 2.8-3.1x sooner. Bounce
level 2 of the dirty faces still waits for the neighbours' level 1 refresh, so full convergence of everything is
unchanged (about 1.05x).
