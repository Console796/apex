# 0.7.2 D2: CPU brush-face charts

2026-11-01. `lighting::chartFace(face, vertices)` uses `geometry::Face` and its owner's vertex
span, without mutating either. It returns the original chart and attributed fragment faces whose
indices address their own vertex arrays. No renderer or existing light-model changes.

## Rules and constants

- **128** is the continuous brush subdivision span for BSP v25, from
  [research.md §4](../../research.md#4-lighting-preview-design) and [0.7 D2](../0.7.md).
  It is `kBrushLightmapSpan`, not a cap of 128 endpoint samples.
- Valve's public v20 [bspfile.h](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/public/bspfile.h)
  calls the brush constants `MAX_BRUSH_LIGHTMAP_DIM_WITHOUT_BORDER = 32` and
  `MAX_BRUSH_LIGHTMAP_DIM_INCLUDING_BORDER = 35`. The unqualified `MAX_LIGHTMAP_DIM_*`
  constants in that header alias the larger displacement limits (125/128).
  D2 uses neither the v20 brush limit nor the displacement limit. Applying the same +3
  storage allowance to v25's 128 would give 131; that is an inference, not a verified
  public Strata constant, and no including-border allocation constant is used here.
- [TexinfoForBrushTexture](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/utils/vbsp/textures.cpp):
  raw VMF axes divided by luxel size, without normalization. Lightmap offsets are
  `textureShift * textureScale / luxelSize`; negative texture scale affects this offset,
  not the lightmap axis direction. Zero texture scale uses 1, as in the compiler.
- [LoadSideKeyCallback](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/utils/vbsp/map.cpp)
  and [vbsp defaults](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/utils/vbsp/vbsp.cpp):
  lightmapscale 0 uses 16; negative values clamp to the default minimum of 1. Positive
  non-power-of-two values are accepted. No global compiler scale/minimum overrides are modeled.
- [CalcFaceExtents](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/utils/common/bsplib.cpp):
  floor the minimum and ceil the maximum of offset projected vertices. BSP extents are
  their difference; sample dimensions are extents + 1. The inverse frame includes both
  the offset and the rounded mins; its local (0,0) lies on the face plane.
- [SubdivideFace](https://github.com/ValveSoftware/source-sdk-2013/blob/master/src/utils/vbsp/faces.cpp):
  test the continuous span without the offset, U before V, split only when it exceeds
  the limit, cut at minimum + limit - 1, and visit front then back. Clipping classifies
  vertices within 0.1 world units as on-plane. Thus an aligned span of 128 has 129 samples;
  a fractionally positioned span of 128 can have extent 129 and 130 samples without splitting.

The implementation uses double precision, matching Apex geometry, rather than duplicating
VBSP float32 rounding, vertex welding or near-integer snapping. Callers supply a valid convex
planar brush face. Invalid indices/nonfinite values, singular axes and integer overflow are
rejected with `invalid_argument`. Displacements and material eligibility remain caller concerns.

## Checks and observed results

- Analytic tests cover 64×64 at scales 16/4, negative coordinates, a sloped face with skew rotated
  axes, signed texture scales/shifts, exact/past-limit and fractional extents, repeated two-axis
  splitting, area preservation, deterministic fragment loops, scale 0/negative/1/3/INT_MAX,
  and malformed input.
- `cmake --build build/linux-clang-release -j` initially failed refreshing zlib:
  `fatal: unable to access 'https://github.com/zlib-ng/zlib-ng/': Could not resolve host: github.com`.
  Reconfigured with `cmake --preset linux-clang-release -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON`; subsequent full build passed. The reference checker
  initially failed to compile because BSPSurfEdge is a wrapper; corrected to `.surfEdge`.
- `QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux-clang-release --output-on-failure
  -R "lighting|chart"`: 9/9 passed (8 chart cases plus an existing renderer lighting case).
- `tests/apex_chart_reference` against the existing native `p2ce/maps/hammertest.bsp` and
  `p2ce/maps/test1.bsp`: **286 lit brush fragments checked, 0 mins/extents mismatches**.
  The checker uses bsppp's decoded face, texinfo, plane and winding data and invokes the real
  CPU chart function. It skips sky/nolight and displacements. Optional FACEIDS identify failures.

The comparison checks already-compiled fragment extents. It does **not** prove VMF axis conversion,
subdivision boundaries, or whole-map fragment equivalence. Those are analytic-only here. Existing
BSP provenance was not regenerated: a fresh native vbsp attempt stalled before compilation in a
Zenity/runtime error dialog (GLib duplicate GListModel errors). No Windows VM was used.

## Repeatable reference procedure

Build as above, then run (read-only; no game files are written):

```sh
build/linux-clang-release/tests/apex_chart_reference \
  "$APEX_P2CE_DIR/p2ce/maps/hammertest.bsp" \
  "$APEX_P2CE_DIR/p2ce/maps/test1.bsp"
```

For fresh reference scenes, use `tools/light_parity.py --keep` with the native runtime configured
as described in [the 0.6.2 reference](lighting_0_6_2_reference.md). Pass its temporary BSP paths to
the checker. It exits 1 for mismatches/no eligible faces, 2 for usage/open errors, and prints both
native and CPU mins/extents on mismatch. Retain BSPs and compile logs when investigating failures.
For subdivision parity, make an isolated `func_detail` brush face of 2048 and 2049 world units at
scale 16, and compare the attributed VMF side's fragment polygons and charts; topology-induced
CSG/portal cuts must be accounted for separately. This last comparison remains undone.
