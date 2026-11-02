# Baker vs in-game set 2 (0.7.4 slice 3, 2026-11-01)

`apex_bake_probe` (tools/bake_probe) bakes `~/maps/apex/test1.vmf` with the CPU baker (direct + sky + 2 bounces,
8 passes, brush geometry only: no props/models, displacements or texlights) and compares it with the in-game
captures in `~/maps/apex/captures/set 2/`. Each pose is a pair: `N1` normally lit, `N2` `mat_fullbright 1`.
Lit / fullbright (sRGB-decoded) stands in for the lightmap; one global scale is fitted in log space.

Poses (cl_showpos, roll 0): 1 (11/12) -1135.97 -1135.97 64.03 / -9.13 34.18; 2 (21/22) -645.80 -1078.80 64.03 /
-7.55 177.66; 3 (31/32) -1046.74 -693.74 64.03 / -7.03 -83.60; 4 (41/42) -1135.97 -973.72 64.03 / -10.20 -9.25.
The 106.26° horizontal FOV assumption aligns: the comparison images overlay edge for edge.

| | Pearson (Y) | baked/game p10 / median / p90 |
|---|---|---|
| direct only | 0.866 | 0.35 / 0.76 / 1.32 |
| direct + bounce | 0.862 | 0.53 / 1.06 / 1.78 |

Fitted scale 2.02 (baker units to display). Bounce lifts the median from 0.76 to 1.06 but widens the spread.
Pose 1 heat map: the ceiling is over-lit and the shaded walls under-lit, so the strips' contribution and the
bounce distribution need work. Shadowed luxels (2.8k) have direct = 0 and get ~13% of the game's value from
bounce. 17 lights baked (5 light, 5 light_rt, 5 light_rt_spot, 2 light_spot).
