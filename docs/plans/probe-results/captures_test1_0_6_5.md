# In-game captures of test1 (0.6.5, 2026-10-29)

Taken by the owner in P2CE from `test1.bsp` (source `~/maps/apex/test1.vmf`; workshop EverythingPBR
materials, two `light_rt` and two `light_rt_spot`, one `env_cubemap`, three `glass_lightcover` strips).
Files live outside the repository in `~/maps/apex/captures/` (1920×1080 PNG). Settings: `cl_showpos 1`,
`r_drawviewmodel 0`, `mat_disable_bloom 1`, `mat_autoexposure_min/max 1` (as requested; not yet
confirmed accepted by the console). `fov_desired 90`, confirmed by the owner. Source defines FOV for 4:3 and widens it for wider screens, so at
1920×1080 the horizontal FOV is 2·atan(tan 45° · (16/9)/(4/3)) ≈ 106.26° and the vertical FOV is
2·atan(tan 45° · 3/4) ≈ 73.74°. Treat this conversion as an assumption to confirm when matching the first frame.

`Pos` is the view (eye) origin: z = 64.03 over a floor at z = 0. `Ang` is pitch, yaw, roll in degrees.

| File | Pos (x y z) | Ang (pitch yaw roll) | Subject |
|---|---|---|---|
| 01 | −1135.97 −720.03 64.03 | −4.89 −58.49 0 | white tile wall under a strip; ribbed black walls |
| 02 | −1135.99 −1071.97 64.03 | −12.91 36.46 0 | room overview, two doors, two strips |
| 03 | −935.61 −913.00 64.03 | −8.34 −13.97 0 | exit door down a corridor, strip above |
| 04 | −754.18 −1071.97 64.03 | −7.28 155.08 0 | white wall, two strips, grazing |
| 05 | −788.30 −912.03 64.03 | −30.57 −157.83 0 | white wall and ceiling, two strips |
| 06 | −1024.01 −1062.15 64.03 | −87.24 −90.74 0 | lightstrip from straight below |
| 07 | −1015.18 −848.58 64.03 | −39.63 −91.79 0 | lightstrip at about 45° |
| 08 | −1005.72 −693.65 64.03 | −26.08 −91.34 0 | lightstrip at a grazing angle |
| 09 | −701.38 −992.22 64.03 | 5.95 −0.09 0 | PBR door close-up |
| 10-def / 10-fullbright | −1135.97 −1071.97 64.03 | −9.55 34.85 0 | overview, normal and `mat_fullbright 1` |
| 11-def / 11-fullbright | −656.03 −1071.97 64.03 | −10.61 145.11 0 | ribbed walls, normal and fullbright |

## Notes

- **`mat_fullbright 1` is not a pure albedo image.** Glossy sheen on the floor and ribbed walls
  survives it (10, 11). Two additive terms can survive fullbright: cubemap reflections (`$envmap
  env_cubemap`, `$envmaptint 0.5`) and `light_rt`, which is rendered dynamically rather than from
  lightmaps. Treat a fullbright capture as albedo plus those terms. Separate them with captures
  that disable one at a time; the game's console names for this have not been checked yet.
- Specular (0.6.5 c) and the light strip (0.6.5 e) are fitted against these views by rendering the editor
  from the same pose. The FOV is recorded above.

## First editor comparison (0.6.5 c, provisional specular)

Editor frames come from `apex_render_pose` at the same pose and FOV (`~/maps/apex/captures/editor/`);
they align with the game frames within 0–2 px. Brightness is the mean of linear values (sRGB decoded with gamma 2.2) over matched regions.

| Region | Game | Editor | Editor/game |
|---|---|---|---|
| lit white floor (06) | 0.189 | 0.140 | 0.74 |
| lit white wall (07) | 0.216 | 0.175 | 0.81 |
| white wall (05) | 0.148 | 0.107 | 0.72 |
| black ceiling away from lights (10) | 0.0048 | 0.0003 | 0.07 |
| ribbed black wall (11) | 0.0093 | 0.0086 | 0.92 |
| black floor tiles (10) | 0.0088 | 0.0061 | 0.69 |
| ribbed wall in shadow (03) | 0.0025 | 0.0025 | 1.01 |
| lightstrip core (06) | 0.647 | 0.193 | 0.30 |

Whole-frame ratios range from 0.40 to 0.77. Directly lit white surfaces are consistently 0.72–0.81 of the
game value. That is a uniform scale, so the cause is exposure or light intensity, not
the falloff shape. Surfaces facing away from every light are black in the editor but faintly lit in the game
(about 0.005). That is indirect light: bounce, which arrives in 0.7, plus the env_cubemap term from 0.6.5 d.
The lightstrip is the largest gap (0.30); it waits for the Refract shader in 0.6.5 e. The render tool still
draws trigger brushes, and the door leaves are missing because doors render in their bind pose.
