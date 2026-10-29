# 0.6.2 irradiance reference: measurements

Everything here comes from compiling small test maps with the P2CE tools (vbsp, vvis -fast, vrad) into temporary
directories under /tmp and reading the resulting BSP. Nothing is derived from the Source SDK or other Valve source.
Lump flag values (SURF_SKY 0x4, SURF_NOLIGHT 0x400, SURF_BUMPLIGHT 0x800) are from public documentation.
The harness is `tools/light_parity.py`. Scratch experiments were throwaway scripts; every result below can be reproduced
with the harness and its options.

## BSP layout (v25, P2CE)

- Header: `'<4si'` followed by 64 lump entries `'<iii4s'` (offset, length, version, fourcc).
- The lumps used:

| Lump | Contents |
|---|---|
| 1 | planes, 20 bytes |
| 3 | vertexes, 12 bytes |
| 6 | texinfo, 72 bytes: 2×4 texture vecs, then 2×4 lightmap vecs, flags i32, texdata i32 |
| 12 | edges, 2×u32 |
| 13 | surfedges, i32 |
| 53 | HDR lighting |
| 54 | worldlights |
| 58 | HDR faces, version 2 |

- **Faces (lump 58).** Each is 72 bytes, `'<IBBHiiiii4BifiiiiiIII'`:
  - planenum u32, side u8, onNode u8, pad u16
  - firstedge, numedges, texinfo, dispinfo, fogvolume
  - styles[4] u8, lightofs i32, area f32
  - lightmap mins[2] i32, lightmap size[2] i32 (luxels = size + 1 on each axis)
  - origFace, numPrims, firstPrim, smoothing
- **LDR is not produced.** LDR faces (lump 7) have the same layout. LDR lighting (lump 8) is empty: VRAD writes only HDR.
- **The referenced plane already faces the lit side** (a ceiling references (0,0,-1,-256)), whatever the side byte says.
- **Lighting at lightofs.** Before lightofs there is one 4-byte average per style: style 0 at ofs-4, style 1 at ofs-8.
  - From lightofs, each style holds **two** maps of w·h luxels in order [map0][map1].
  - map0 is the lightmap.
  - map1 is all zero under -bounce 0. With bounces (and -aoscale 0) it averages about 3.49× the direct model. Its purpose is unresolved, and the harness reads map0 only.
- **Named lights** land in style 32.

## Decode and units

- Each luxel is 4 bytes, ColorRGBExp32: r, g, b u8 and exponent i8. Value = m · 2^e / 255 per channel.
- The units are the worldlight lump's intensity units directly. Measured cases:
  - A sun (light_environment, I = 1) straight down onto an open floor stores exactly 255/255.
  - At pitch -60 the same sun stores 206/255 = 0.80784, against a model value of 0.80801. So storage **truncates**.
- Mantissas are normalised into [128, 255].
- The harness decodes at the middle of the bucket, (m + 0.5) · 2^e / 255. An all-zero luxel decodes as 0.
- **Quantization.** The resulting step is under 0.4% of the brightest channel. For a dim minor channel of a coloured light it is much more, because all three channels share one exponent.

## Luxel positions and sampling

- **Position.** Luxel (u, t) of a face sits at lightmap coordinate s = u + mins_s (likewise t). The world point solves lm_s·p + lm_s.w = s and lm_t·p + lm_t.w = t on the face plane.
  - There is **no** half-luxel offset. Symmetric scenes and a shadow edge both confirm this.
- **-noextra** (one sample cell per luxel) is not a point sample. The luxel equals the mean of 4 samples at (s ± ½, t ± ½), each **lifted 1 world unit along the normal**.
  - Fits of a 4-corner model (offset ±a world units, lift dz) on a point-light floor:

    | lightmapscale | best a | dz | sd of ratio |
    |---|---|---|---|
    | 16 | 8 | 1 | 0.16% |
    | 8 | 4 | 1 | 0.17% |
    | 32 | 16 | 1 | 0.16% |

  - Without the lift, the far field reads about 1 − 1/h low.
  - A shadow edge reads exactly 0.5 whatever the edge's phase within the luxel.
- **Default compile (supersampling on).** It differs from -noextra only where the gradient is high: cone edges and shadow edges. There it pushes the spot scene's p99 to 7.3% (`--supersample`).
- **-final** is identical to the default.
- **-fast** changes the sampling (peak +1.5%) and should not be used for references.
- **Baked AO is on by default** in this VRAD. It multiplies direct light near concave edges:
  - luxels 0, 1 and 2 from the edge read 0.46, 0.63 and 0.91 of the model;
  - it is gone by about 3 luxels.
  - `-aoscale 0` (or `-aosamples 0`) removes it. With AO left on (`--ao`), the point scene's p99 is 78%.
- **-bounce 0** is accepted and removes indirect light. map0 / model is 0.997 with it and 1.23 without.
- **Nothing else adds light.**
  - A sealed room with only a light_environment, or with no light at all, has zero nonzero luxels.
  - Sky ambient reaches only faces that can see sky faces, as measured below.

## Measured model (now in src/lighting/light.cpp)

- **Surface response is w(c) = c(1 + c)/2, not Lambert's c.**
  - It holds for point lights on floors and walls, for the sun, and as the weighting of sky ambient.
  - It does not depend on the material (dev/reflectivity_50, dev/graygrid, concrete).
  - The 0.6.1 Lambert model is biased **+19% to +34%** high in every scene (`--model 0.6.1`).
- **Point.** I · w(cos) / (c + l·d + q·d²), cut off at `_distance`. Residual ≤ 0.5%.
- **Spot.**
  - Inside the inner cone the factor is `along` (the cosine to the axis): a cosine emitter. The exponent makes no difference there: 0, 1, 2 and 5 are identical.
  - The surface cosine was separated on walls: S/along matches within about 0.5%.
  - Between the cones: along · lin^e, with lin = (along − stopdot2)/(stopdot − stopdot2). Exponents 0 and 1 both mean linear.
  - Fitted exponents on the floor: 0.5 → 0.495, 2 → 2.04–2.07, 5 → 5.06–5.14. On walls they fit 1.86 for exponent 2 and 2.69 for exponent 3, attributed to sampling blur across a narrow ramp.
  - Zero outside the outer cone.
- **Sun.** I · w(n·−dir) where the sky is visible, 0 otherwise.
- **Sky ambient.** I_amb · ∫_sky w(cos) dω / (5π/6).
  - An open floor under full sky reads exactly I_amb.
  - In a pit test, the w weighting fits within about 1%. A cosine weighting is 12–15% off.
  - Every sky face counts, wherever it is.
  - Closed form for the full upper hemisphere, on a face with normal z:
    F(z) = 0.3(1 + z) + 0.2 + (2/(5π))(asin z + z·√(1 − z²)).
  - Values: F(1) = 1, F(0) = 0.5, F(−1) = 0, F(0.5) = 0.77179.
  - Checks: a pillar base reads 0.502–0.511, and a ramp top is within 0.3% of the integral.
  - This is `skyAmbientResponse`. Partial sky must still scale it by visibility (0.6 D16).

## Harness

```
python3 tools/light_parity.py                       # all scenes, gated: mean 2%, p99 6%
python3 tools/light_parity.py --scene spot --report-only
python3 tools/light_parity.py --fixture tests/fixtures/lighting/irradiance_parity.json
```

- **Requirements.** It needs `$APEX_P2CE_DIR` and the native tools. It compiles in a temporary directory under /tmp and never writes into the game.
- **VRAD arguments.** `-bounce 0 -noextra -aoscale 0` (the sky scene adds `-extrasky 64`).
- **Options.**
  - `--model 0.6.1` evaluates the old Lambert model.
  - `--sampling centre` uses one sample at the luxel centre.
  - `--supersample` drops `-noextra`; `--ao` drops `-aoscale 0`.
  - `--keep` keeps the compile directory.
- **Masking.**
  - Faces skipped: no lightmap, displacement, sky and nolight faces. Bumped faces count as excluded.
  - Luxels whose ±1-luxel square leaves the face polygon are dropped (the face-edge mask).
  - Luxels where any of the 4 samples cannot see a light origin, or the sky along the sun, are dropped. That blocked set is dilated by 1 luxel.
- **Error.** Rec. 709 luminance err = |model − ref| / max(ref, 1% of the scene's p99 reference). Luxels where both values are below that floor are counted as "dark" and left out of the statistics. The harness also reports per-channel errors.
- **Exit.** Non-zero above mean 2% or p99 6%, unless `--report-only`.
- **Fixture.** `--fixture` writes up to 32 samples per scene: the 4 lifted sample points, the normal and the reference luxel. The sky scene is not written, since ambient needs visibility. `tests/lighting/light_test.cpp` replays the fixture against `irradiance()` within 1% of each luxel's brightest channel.

### Results (measured model, default sampling, about 47 s)

| scene | contents | excluded | mean | p99 | max | n | dark |
|---|---|---|---|---|---|---|---|
| point | coloured point, inverse square | 11.8% | 0.157% | 0.431% | 0.519% | 15376 | 0 |
| spot | 25/40°, exp 1, pitch -70 | 11.8% | 0.139% | 0.342% | 0.377% | 2161 | 13215 |
| spot_exp | 20/45°, exp 3, pitch -20 | 11.8% | 0.137% | 0.340% | 0.378% | 1906 | 13470 |
| fifty | 50% at 200, 0% at 600 | 11.8% | 0.143% | 0.346% | 0.380% | 15376 | 0 |
| attn | c/l/q 1/0.02/0.0005 plus a linear coloured light | 11.8% | 0.135% | 0.394% | 0.535% | 15376 | 0 |
| occluded | point beside a pillar | 28.5% | 0.140% | 0.351% | 0.389% | 12412 | 868 |
| sun | courtyard, pitch -55 | 34.0% | 0.103% | 0.171% | 0.171% | 3156 | 2852 |
| sky | courtyard, ambient only | 26.2% | 0.583% | 2.571% | 3.729% | 1804 | 0 |

- **Worst channel.** The point scene's blue channel: mean 0.46%, p99 1.09%. This is quantization of a minor channel.
- **Evidence runs.**
  - `--model 0.6.1` fails: the point scene's mean is 18.8% and p99 49%; spot mean 32.4%; sun 34.3%; sky mean 5.4%, p99 12%.
  - `--sampling centre`: point mean 0.22%, but spot p99 41% at the cone edges.

## Unresolved

- **The second map per style** (zero under -bounce 0) is not explained.
- **Ambient near concave edges** carries a ±1% residual. VRAD is brighter on a ramp near the floor (+7% at 16 units), which looks like extra sky sampling near geometry.
- **Default compiles bake AO**, and the editor model does not have it. A parity bake must either pass `-aoscale 0` or model AO.
- **In-game check not done.** The D3 reversal check (an in-game capture of a -bounce 0 scene) needs the game running.
