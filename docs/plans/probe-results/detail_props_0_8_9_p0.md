# 0.8.9 P0: VBSP's detail-prop placement

Plan 0.8, sub-milestone 0.8.9 (D16). The preview has to place detail props where VBSP does. The owner chose
a synthetic oracle (2026-11-15): compile a map with our own detail.vbsp and match the compiled `dprp` lump.

## Nothing to probe in the install

- No `detail.vbsp` ships with P2CE (p2ce, hammer or portal2). No material sets `%detailtype`.
- No corpus map has a displacement (`dispinfo` occurs in 0 of 510 VMFs; see `structural_0_8_9_p0.md`).

So `tools/detail_probe.py` brings everything: a scratch game directory whose `gameinfo.txt` searches
itself and then the real install by absolute path, two `%detailtype` materials, a `detail.vbsp` with two
types (alpha-blended groups, sprite, cross shape, upright, slope limits, random scale, sway), and a sealed
room with a power-3 displacement and a flat block.

## The native Linux vbsp never places a detail prop

`bin/linux64/vbsp` (Apr 23 2026) imports glibc `rand`/`srand` (`objdump -T`) and divides every draw by
the float at `0x1a6864`, which is `0x46fffe00`, 32767.0 (MSVC's `RAND_MAX`). The divisions are at
`0x1bf43e`, `0x1bf564`, `0x1bf937`, `0x1bfa74` and `0x1c0c8f` (sampling, group and model choice,
PlaceDetail). glibc's `rand()` goes up to 2³¹−1, so the "unit" draws are mostly far above 1, and
SelectDetail's `r <= amount` almost never holds. Three Linux compiles of the probe wrote a `dprp` lump with
no objects; the same VMF under the Windows `bin/win64/vbsp.exe` (Wine, `WINEDEBUG=-all`, `Z:` paths)
places hundreds.

Consequence: a map compiled with P2CE's Linux tools has no detail props in game. Apex's preview follows the
Windows compiler, which is the one that works; the as-built notes and the D15 scenario must not claim the
preview matches what a Linux compile ships. Worth reporting to Strata.

## The Windows vbsp is deterministic

Three Wine runs of the probe gave byte-identical `dprp` lumps: 949 objects and 4 sprite dictionary
entries with the flat block, 901 objects and 3 entries with the displacement alone. VBSP seeds MSVC
`srand` and vstdlib's `RandomSeed` with the face's Hammer side ID before each `%detailtype` face, so the
placement is a pure function of the map and detail.vbsp.

## The rule

The structure of Strata's vbsp, as read from the Linux disassembly, matches SDK 2013's
`utils/vbsp/detailobjects.cpp`, `public/builddisp.cpp` and vstdlib's `random.cpp`; Apex ports those
(`src/geometry/detail_props.cpp`). Per displacement face:

1. `srand(sideId)`, `RandomSeed(sideId)`. MSVC `rand`: `state = state * 214013 + 2531011`,
   `(state >> 16) & 0x7fff`.
2. Samples = `(int)((float)(baseArea * density) * 1e-6)`, baseArea the two triangles of the base quad.
3. Per sample: `u`, `v` = `rand() / 32767.f`; DispUVToSurf picks the grid triangle by cell parity and
   flat-plane barycentrics, giving point, triangle normal and alpha (÷255).
4. SelectGroup: the two groups (sorted by alpha) the alpha lies between; one more draw picks between them
   by distance. SelectDetail: one draw against the cumulative amounts.
5. PlaceDetail: drop if `normal.z` is below cos(maxAngle); between the two limits, one draw for a coin
   flip; one draw for yaw (upright) or rotation about the normal; sprites with `spriterandomscale` take
   `|Gaussian(1, stddev)|` from vstdlib's stream.

Not reproduced: flat (non-displacement) faces. VBSP walks the compiled, fragmented BSP faces with their
T-junction vertices, re-seeding each fragment with the same side ID, which the editor can't reproduce. D16
scopes the preview to displacements. Also `func_detail_blocker` (Strata collects them before placement)
is not modelled.

## How exact

The committed oracle is the displacement-only compile (`tests/fixtures/detail/`, `--fixture`), checked by
`tests/document/detail_props_oracle_test.cpp`: same count; each object matched by model and origin within
1e-3; orientation compared as vectors (angles alias); sway, shape, orientation and type exact. Sprite scale
is exact only on the first `%detailtype` face VBSP compiles: the Gaussian stream keeps a cached second
value across faces and `RandomSeed` does not clear it. Every other field is exact per face.
