# Bake tuning against set 2 (0.7.4, slice 5)

Command: `apex_bake_probe --vmf ~/maps/apex/test1.vmf --game $APEX_P2CE_DIR --captures "~/maps/apex/captures/set 2" --passes 16 --bounces 4 --bounce-rays 64 --stride 16`

## Finding
Bounce depth and ray count were not the cause of dark areas: 2 -> 8 bounces and 32 -> 128 rays left the
shadowed subset at 0.13-0.15 of the game. The cause was the bounce albedo. VBSP stores the VMT's
`$reflectivity` override in texdata, and some materials (notably the PBR black tiles) override it far
above the base VTF's average colour. `setupBake` now applies that override, so bounce carries the
energy the game's VRAD carried.

## Before / after (clean samples, baked x fitted scale / game)
| subset | before (scale 2.02) p10/med/p90 | after (scale 0.78) p10/med/p90 |
|---|---|---|
| all | 0.53 / 1.06 / 1.78 | 0.75 / 1.02 / 1.33 |
| shadow direct+bounce | 0.08 / 0.15 / 0.20 | 0.40 / 0.86 / 0.97 |

The fitted display scale dropped to 0.78 (default `render::kDefaultBakedScale`), since bounce now supplies
energy the earlier fit had folded into the scale.

## Baked specular
The baked branch of `brush_textured.frag.glsl` now runs the Lit light loop for specular only (same
shadows), adds it to lightmap diffuse and keeps envmap/self-illumination. `apex_render_pose --no-specular`
disables highlights for image comparisons. Verified on llvmpipe only; not checked on the RX 6600.
