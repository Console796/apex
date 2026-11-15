# 0.8.5 P0: displacement lightmaps in VBSP and VRAD

Plan 0.8, sub-milestone 0.8.5. Measured 2026-11-14 with the native P2CE `vbsp` and `vrad`
(`$APEX_P2CE_DIR/bin/linux64`, Oct 19 2026 build) on variants of
`tests/fixtures/vmf/disp/test1_disp.vmf` (side 355, power 3, base x −96..96, y −1632..−1440, z 64,
`lightmapscale` 16, axes `[1 0 0] 0.25` / `[0 -1 0] 0.25`). Each variant is sealed in the
`tools/disp_oracle.py` hull. A lit variant adds one `light` (`_light 255 255 255 300`) and runs
`vrad -bounce 0 -noextra`. The BSP (v25) was read with a small Python reader in the probe's
scratch directory (not kept). `tools/disp_lightmap_oracle.py` regenerates the subset that the
tests check (`tests/fixtures/vmf/disp/test1_disp.lightmap.txt`).

59 compiled variants. The rules below match every one: 59/59 extents, 54/59 mins (see Mins).

## The base face comes from `point_data`

VBSP builds a displaced side's base face from the side's `point_data`, not from the brush planes.

- Moving a plane but not `point_data` changes nothing: same face, same extents.
- Moving `point_data` but not the plane moves the BSP face: extents (13, 11).

Apex rewrites `point_data` from the polyhedron when it writes a side (`document.cpp` `writeFace`),
so a file Apex saved agrees. A hand-edited or third-party file where the two disagree compiles to a
different base quad than the one Apex meshes and charts. Apex charts from the planes. Not handled:
this is a D2 note for the plan owner.

## Luxel scale: truncated in float

The luxel size VBSP uses for a displacement is `int(1.0f / |lightmapVecs[0]|)`, computed in
float. `lightmapVecs[0]` is `uaxis / lightmapscale`, and `uaxis` is taken as written (not
normalised). Float error truncates scales 7, 13, 14 and 15 to 6, 12, 13 and 14. Scales 1, 2, 4, 9,
10, 11, 12, 16, 17, 18, 20, 24, 25, 32 and 48 are exact. Only the s vector counts: a skewed
`vaxis` changes nothing. `uaxis [1 0 0.5]` at scale 16 gives 14. `lightmapscale "15.5"` behaves
as 15, so the parse is an integer one.

## Extents

Write the base quad in Source grid order: c0 is the corner nearest `startposition`, rows (i) run
c0→c1, columns (j) run c0→c3, as in D2's DISP_VERTS. Then:

- `extents[0] = floor(max(|c0c3|, |c1c2|) / scale) + 1`, along the columns (j).
- `extents[1] = floor(max(|c0c1|, |c3c2|) / scale) + 1`, along the rows (i).
- The lengths are world lengths. The lightmap axes' directions do not enter.
- There is no 128-luxel subdivision and no clamp: scale 1 gives 193 × 193.
- The extents follow the grid, not the texture axes. Moving the start corner by one on a 192 × 160
  base swaps them, from (13, 11) to (11, 13), with the texture axes unchanged.

| Edge length (luxels) | 4 | 6 | 8 | 10 | 10.67 | 11.375 | 11.875 | 12 | 12.375 | 12.75 | 12.86 | 13.71 | 19.2 | 32 | 48 | 192 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| extents | 5 | 7 | 9 | 11 | 11 | 12 | 12 | 13 | 13 | 13 | 13 | 14 | 20 | 33 | 49 | 193 |

On a trapezoid base, the longer of the two parallel edges counts: edges 10 and 12 give 13.

## Mins

The mins are the flat base face's own, as for a brush face: `floor(min over the loop of p·lightmapVecs + offset)`.
The shift variant has an st minimum of (−5.5, 90.25), which gives (−6, 90). The 5 misses all come
from the probe's float arithmetic: an exact integer minimum came out as −8.0000003 and floored one
too low. VBSP does not make that error, and Apex's `chartFace` (double) does not either.

## Sample positions and orientation

DISP_LIGHTMAP_SAMPLE_POSITIONS (lump 34), as decoded:

- Per luxel, row-major with s fastest: one triangle byte (255 would escape to a second byte, never
  seen), then three barycentric bytes.
- The three barycentric bytes sum to 254 or 255. Divide by their sum.
- Triangles are numbered in a quadtree order, not DISP_TRIS order. The children of a node are, in
  order: (i low, j high), (i low, j low), (i high, j low), (i high, j high). A 2 × 2 cell block holds
  8 triangles. The triangles themselves are D2's alternating-diagonal grid.

Luxel (s, t) samples the displaced surface at grid fraction (row t / extents[1], column
s / extents[0]): a uniform parametric grid over the base quad, edge to edge. It is bilinear on a
trapezoid. There is no half-luxel offset, and it is neither the planar chart lattice nor a stretch
of the st bounding box. Checks:

- Base: 196/196 luxels within 0.004 luxel.
- Every non-square, trapezoid, skewed and scale variant: within 0.012 luxel.

The sample lump alone cannot tell the eight symmetries of the square apart, so orientation came from
lit compiles. Each fit compared VRAD's luxels with a direct-light estimate under all 8 symmetries.
The best fit had r = 0.84–0.94, and the next best 0.49–0.82. 18 lit variants:

- base, `startposition` at the opposite corner, a texture shift, rotated axes (37°), rotated axes
  at scale 10;
- a 192 × 160 base: as-is, with two other start corners, with rotated axes, with axes rotated 90°,
  and at scale 15;
- a trapezoid: as-is, and at scale 15 as-is, with two other start corners, and with rotated axes;
- scales 15 and 4.

In every one, luxel (0, 0) is at c0 (the start corner), s runs along the columns (c0→c3) and t
along the rows (c0→c1). Rotating the texture axes by 90° does not change this. Orientation follows
the start corner, not the lightmap axes.

## What VRAD stores

The face's lightmap holds (extents[0] + 1) × (extents[1] + 1) luxels, at the face's `lightofs`,
style 0 first, like a brush face. The BSP face carries the base quad's four vertices and the side's
texinfo, so a fragment maps back to its side by its plane and its four corners.

## Consequences for 0.8.5

- Charting: as above. No subdivision, so one BakeFace per displaced side. The planar part of the
  chart (`worldToLuxel`, offset, mins) is the side's. `luxelToWorld` and `origin` do not locate luxels.
- Bake: sample luxel (s, t) on the displaced surface at (t / extents[1], s / extents[0]), lifted
  along the surface normal there. The baker's four corner samples at ±0.5 luxel clamp to the grid.
- Lightmap UVs of the displaced vertex (i, j): luxel coordinate (j / (n − 1) · extents[0],
  i / (n − 1) · extents[1]). This is not a projection.
- VRAD import: the displacement fragment's luxels are already on Apex's grid when the extents
  agree.

## Not measured

- Whether displacements cast shadows in VRAD (the D8 caster probe). Not run here; the baker keeps
  today's casters.
- The normal VRAD shades a displacement luxel with. It is assumed to be the smoothed vertex normal.
  The fits use flat triangle normals and still reach r ≈ 0.9.
- A displacement with power 2 or 4, and a base face that is not horizontal. Only test1_disp
  (power 3, horizontal) was compiled.
- `lightmapscale` values that are not integers, beyond 15.5.
