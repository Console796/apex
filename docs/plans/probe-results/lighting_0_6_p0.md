# Lighting 0.6 P0: light keys to worldlights — 2026-10-29

Native P2CE Linux VBSP, VVIS `-fast` and VRAD `-fast` compiled generated sealed rooms
(up to 64 lights per map, 640 units apart, matched back by origin). No Valve SDK code
was consulted; the VDC wiki was unreachable (HTTP 403), so everything below is measured.
Rerun: `APEX_P2CE_DIR=... python3 tools/light_probe.py [--group color|attn|falloff|falloff_sweep|spot|misc|rt|env] [--vrad-arg=-finitefalloff] [--raw]`.

## Lump layout (BSP v25, lump 54 v2)

Only `LUMP_WORLDLIGHTS_HDR` (54) is written; lump 15 stays empty with `-ldr`, `-both`
and `-final`. Records are 100 bytes (1800 bytes / 18 lights, etc.), little-endian:

| off | type | field | evidence |
|---|---|---|---|
| 0 | 3f | origin | equals entity origin |
| 12 | 3f | intensity | see below |
| 24 | 3f | normal | spot/sun direction; garbage for point lights |
| 36 | 3f | shadow_cast_offset | `_shadoworiginoffset "1 2 3"` → (1,2,3) |
| 48 | i32 | cluster | small leaf-cluster ints |
| 52 | u8×4 | type, specular, direct, indirect mode | `_specularmode/_directmode/_indirectmode` bytes |
| 56 | i32 | style | `style 10` → 10; targetname → 32, 33… (shared name shares) |
| 60 | f×7 | stopdot, stopdot2, exponent, radius, constant, linear, quadratic | |
| 88 | i32 | flags | 0x2 default; `_castentityshadow 0` → 0; sun records 0 |
| 92,96 | i32 | texinfo, owner | 0 |

Type: 1 point, 2 spot, 3 skylight, 5 skyambient.

## Colour and intensity

`I = (C/255)^2.2 · (A/255) · S`. Measured G=255 with A=100/200/400 gives 3921.57/7843.14/15686.3
(S=10000). R=128 → 0.21952 (=(128/255)^2.2), R=64 → 0.047776, R=0 → 0. A 3-value `_light`
uses A=255; a 1-value `"128"` is grey 128, A=255; a missing `_light` gives 0.
The HDR lump uses `_lightHDR` when it is not the `-1 -1 -1 1` sentinel; otherwise it uses `_light`.
It then multiplies by `_lightscaleHDR`: `255 0 0 50` → (1960.78,0,0), and scale 2 → 15686.3.

## Attenuation

The constant, linear and quadratic keys are stored as given. Intensity is normalised at 100 units:
`S = c + 100·l + 10000·q` (c0 l0 q1 7843.14; c1 l1 q1 7922.35; l0.5 q0.25 2000).
All-zero coefficients, or c=-1, are stored as c=1. Then S=1.

`_fifty_percent_distance` f>0 overrides c/l/q, and S=1 (I=0.784314 at A=200).
With r = z/f, where z=`_zero_percent_distance` and z<f (including 0) means r=2. z=f exactly is
its own case: c=0, l=2/f, q=0 (purely linear; `50%=100 0%=100` → l=0.02):
- **exact**: `c + l·f + q·f² = 2` and `(c + l·z + q·z²)/c = 256` (all 60+ cases)
- scale-invariant in f: 64/256, 100/400 and 256/1024 all give c=0.113074
- **resolved (dense sweep, `--group falloff_fine0..4`, 300 ratios at f=1000, plus the 43-ratio sweep at
  f=100):** for n = 0, 1, …, 19 take
  `c = 1 / (1 + n·(255 − r)/(40·r))`, solve the two equations above for l and q, and stop at the first n
  whose curve is not brighter one world unit from the light than at it, i.e. `l + q ≥ 0` in world units
  (n = 19 if none passes). n = 0 gives c = 1, the r ≳ 16 case. All 343 measured lights reproduce with
  the attenuation curve within 1e-4 relative over [0, 1.2z]. Because the test is in world units, the
  n=0/1 boundary moves slightly with scale (r=15.9 is n=0 at f=100 and n=1 at f=1000), which is what
  the measurements show. The earlier "sawtooth" in l·f/c is the jump in n at each boundary.

`_distance` is copied to the radius field (300 → 300); 0 means none. `_hardfalloff`,
`_hard_radius_override` and `_hard_radius_threshold` do not change the record.
`_zero_percent_distance` without fifty is ignored.

With `-finitefalloff`, every point light gets c=1, l=0, q=−1/z² (z=400 → −6.25e-6;
no z → −inf), S=1 and radius 0. `-final` matches `-fast`.

## Spot

stopdot=cos(`_inner_cone`), stopdot2=cos(`_cone`) (30/45 → 0.866025/0.707107; 10/20; 60/80).
An inner value of 0 or missing becomes 10° (0.984808). If cone<inner, or the cone is 0 or missing,
cone=inner. Exponent is stored as given; when missing it is 0 (not the FGD default 1). Intensity
normalisation is the same as for point lights.

Direction: `p = pitch` key if nonzero, else `angles[0]`, y=`angles[1]`, roll ignored.
`n = (cos p·cos y, cos p·sin y, sin p)`. Pitch −90 → (0,0,−1); −30 yaw 90 → (0,.866,−.5);
pitch 20 → z=+0.342; pitch −30 overrides angles 60; angles −45 without pitch → z=−0.707.

## light_environment

It writes two records at the entity origin, even without sky faces:
- skylight: `_light`/`_lightHDR`/`_lightscaleHDR` colour with no 100-unit scale (A=200 → 0.784314),
  direction n by the spot rule (pitch −45 yaw 30 → (.6124,.3536,−.7071)), c/l/q 0, flags 0.
- skyambient: `_ambient`/`_ambientHDR`/`_AmbientScaleHDR` by the same rule (20 → 0.0784314;
  `0 0 255 40`×3 → 0.470588), normal 0.

`SunSpreadAngle` (0, 5, 20) does not change either record.

## light_rt

The classname does not matter. Mode keys go into bytes 53–55 as specular/direct/indirect,
whatever the entity class. Defaults are 0/1/1: `light` and `light_rt` with no keys are identical.
The FGD defaults for `light_rt` (d2 i1 s2) → 2/2/1; d0 i0 → 0/0/0.
Spawnflags 2 (shadowed) and 1 (initially dark) do not appear in the record.
`_removeaftercompile 1` removes the light entirely (no worldlight) for `light` and `light_rt`.

## Ambiguities

- Point-light normal bytes are non-deterministic.
- How c<0 is handled when l or q is non-zero (c=-1 with l=q=0 is stored as c=1).
- Colour parsing outside the probed forms: two-value colours, partial `-1` sentinels, negative channels or
  brightness, malformed tokens. `lighting::lightsFromEntity` currently treats two values as grey, clamps
  negative channels and needs all three of R, G and B at -1 for the sentinel; none of that is measured.
- l and q of the 50%/0% solve match the lump to the curve (1e-4) and c to float precision, but not
  bit for bit: VRAD's float32 operation order is not reproduced.
- The lump has no LDR data, so `_lightHDR` only matters through the HDR lump.
