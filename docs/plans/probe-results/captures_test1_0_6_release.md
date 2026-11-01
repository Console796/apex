# In-game captures of test1 for the 0.6 release review (2026-11-01)

Taken by the owner in P2CE from `test1.bsp` (source `~/maps/apex/test1.vmf`). Files live outside the
repository in `~/maps/apex/captures/`, 1920×1080 PNG, `fov_desired 90`. Each pose has a lit frame and
a `mat_fullbright 1` frame.

- **`set 1/`** (`N-1.png` lit, `N-2.png` fullbright): **auto exposure**, not pinned. Kept for reference
  only; the game brightens these dim rooms, and Apex Lit came out at about 0.6× of it overall.
- **`set 2/`** (`N1.png` lit, `N2.png` fullbright): exposure pinned with `sv_cheats 1`,
  `mat_autoexposure_min 1`, `mat_autoexposure_max 1`, `mat_force_tonemap_scale 1`,
  `mat_disable_bloom 1`, `r_drawviewmodel 0`, `cl_showpos 1`. This is the set the release reviews.

| Set 2 pose | Pos (x y z) | Ang (pitch yaw roll) |
|---|---|---|
| 1 | −1135.97 −1135.97 64.03 | −9.13 34.18 0 |
| 2 | −645.80 −1078.80 64.03 | −7.55 177.66 0 |
| 3 | −1046.74 −693.74 64.03 | −7.03 −83.60 0 |
| 4 | −1135.97 −973.72 64.03 | −10.20 −9.25 0 |

## Comparison with Apex Lit

Editor frames come from `apex_render_pose --mode lit|fullbright` at the same pose (linux-clang-release,
RX 6600, map exposure 1). Rendering pose 1 and pose 2 of set 1 at FOV 70–90 confirmed FOV 90 aligns
(edge correlation 0.77 and 0.90 against at most 0.22 for the others).

Ratios are Apex ÷ game, linear (sRGB decoded), with the HUD and crosshair masked. "Bright" is the median
per-pixel ratio over the brightest fifth of the game frame (mostly direct light), "dark" over the darker
half (where bounce matters).

| Pose | Lit mean | Lit bright | Lit dark | Fullbright median |
|---|---|---|---|---|
| 1 | 0.73 | 0.66 | 0.86 | 1.38 |
| 2 | 0.80 | 0.78 | 0.87 | 1.40 |
| 3 | 0.75 | 0.69 | 0.93 | 1.34 |
| 4 | 0.76 | 0.72 | 0.84 | 1.35 |

- Pinning exposure moved Lit from about 0.6× to about 0.75×.
- Direct-lit regions remain 22–34% dim in Apex. Missing bounce (D8) does not explain that part.
- Apex fullbright is 1.34–1.40× the game's, but the game's fullbright keeps envmap and `light_rt`
  terms (see captures_test1_0_6_5.md), so it is not a clean albedo reference.
- The exit sign's running-man decal and the door window decal are missing or white in Apex; they are
  among the six materials that fail to load for test1.

## Investigation (2026-11-01): two errors that partly cancel

**Bounce is about half of the game's baked light.** test1 was compiled by the editor's Normal
profile (`vrad -StaticPropLighting -StaticPropPolys`, default bounce). Relighting that same BSP with
`-bounce 0` (same flags, the editor's generated `content/gameinfo.txt` so the workshop materials mount)
gives a style-0 lightmap whose total luminance over all non-displacement luxels is 0.49 of the game
BSP's. Even the brightest directly lit luxels are only about 0.56 direct (median). Apex shows direct
light only (D8), so its lit frames should come out at roughly 0.5–0.6× of the game from bounce alone.

**Apex's display curve is still the placeholder.** `tonemap.frag.glsl` encodes with
`pow(x·exposure, 1/2.2)` ("placeholder display curve until the game's curve is fitted"). Against set 2's
fullbright frames, Apex is brighter by an amount that depends on level, not by a constant: about 2.6×
(linear) at sRGB 10–30, 1.4–1.5× at 30–70, 1.1–1.2× at 110–190, and slightly darker above 210. A
power fit gives Apex ≈ game^0.85–0.9 in display space. Part of this may be material response rather
than the curve: most of test1 is EverythingPBR, and the game's PBR shader under `mat_fullbright` may
still darken metallic diffuse, while Apex fullbright reproduces albedo.

Together: about 0.55× from missing bounce, times about 1.35× from the curve and material response,
gives the 0.75× observed. Neither the light model nor exposure is shown to be wrong.

**Next capture to separate them (D3's still-open in-game `-bounce 0` capture).**
`build/parity/test1_b0.bsp` is test1 relit with `-bounce 0`. Captured in game at the set 2 poses with the
same pinned settings, Apex Lit should match it in direct-lit regions once the display curve is right,
and the remaining difference measures the curve and material response by themselves.
