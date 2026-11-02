# CPU bake parity probe

Build `apex_bake_probe` with `APEX_BUILD_TESTS`. No GPU, window or Qt application is created.
It loads `--game` (or `APEX_P2CE_DIR`) exactly as render_pose does: `p2ce/gameinfo.txt`
and the game's enabled addon list. Instance resolution uses the same search roots; document
collapse supplies recursive transforms and key fixups. Neither path uses an FGD class table.
`--no-assets` skips VFS/material reads, uses linear reflectivity 0.5 and name-based flags,
but still uses the game directory to find instance VMFs. Unresolved instances fail explicitly.
Individual failed materials/textures are reported and use that same fallback.

The scene includes world and func_detail geometry, including instance contents. Props/models,
other brush entities, displacements and hidden geometry are out of scope. Sky and nodraw
remain casters; other `tools/` faces are omitted. Material `%compilesky`, `%compilenodraw`,
and `%compilenolight` are honoured; sky/nodraw names always apply. Albedo comes from the
base VTF's **linear reflectivity**, without preview textures, gamma decoding or `$color2`.
This is a VMF brush approximation, without BSP CSG, texlights or runtime material lighting.

Set 2 has four fixed poses and pairs `11.png/12.png` through `41.png/42.png`. The first
is lit, the second fullbright. All captures must be 1920x1080. Source FOV 90 at 4:3
becomes 106.26 horizontal / 73.74 vertical here, using viewport::Camera. Pixel centres
use forward + screen-right + screen-up. Centre diagnostics use the exact optical axis.
Alignment fractions distinguish any caster hit from hits mapped to a bake chart.
Props and overlays in the capture can still mismatch brush rays; the diagnostics do not
prove alignment. Inspect the images and printed centre hits before interpreting parity.

`--passes` is a fixed accumulation count for direct, then each bounce level in order,
with no adaptive convergence claim. Defaults are 16 passes, 2 bounces, 32 bounce rays,
3 worker threads, seed 0, and a stride of 8 pixels for statistics. Direct passes repeat
deterministically except for sky sampling; bounce passes progressively sample transport.

PNG channels use the piecewise sRGB display curve in both directions. For each sampled
mapped hit, nearest chart luxel radiance is compared with per-channel linear lit/fullbright.
Pixels with **any fullbright channel < 0.02** are skipped. A simple contamination flag marks
any fullbright channel > `2 * VTF reflectivity + 0.05`, or any linear lit channel > 0.8.
These flags identify possible reflections, dynamic lights or emissive strips, not their cause.
The mask can also reject texture detail above mean reflectivity, and miss weak additive terms.
Flagged pixels remain in the CSV but are excluded from fitting and reported statistics.
No HUD-specific mask is applied.

Exactly **one scalar per run** multiplies baked radiance. It is
`exp(mean(log(gameRatio[c]) - log(totalBake[c])))` over positive (>1e-8), clean sampled
RGB channels in all poses. This minimizes squared log residuals. The same scalar is used
for direct-only and direct+bounce, Y/R/G/B, every pose and lighting subset; direct is not
refitted. Rec.709 Y is computed from the RGB ratio (not a ratio of luminances). Pearson is
computed in linear space. Percentiles use linear interpolation of scaled-bake/game ratios.
Zero baked values remain zero in these statistics; game ratios <=1e-8 are excluded for
that metric. Empty groups or constant-valued Pearson inputs report nan. `direct-lit` means
raw direct Y >1e-6; `shadow` is the complement. This split is numerical, not a visibility label.

Each `poseN-samples.csv` contains raw unscaled direct/total RGB, linear PNG RGB, ratios,
pixel/fragment/solid/side identity, world hit, contamination and shadow flags. The scale,
alignment checks and statistics are saved to `summary.txt`. `poseN-compare.png` is 5760x1080:

1. Game lit.
2. `scale * totalBake * fullbright`, sRGB encoded and clipped for display (all valid mapped pixels).
3. Luminance log2 error: green at parity, red at +2 stops or more, blue at -2 stops or less;
   purple means unmapped, low fullbright or contaminated. Prediction is black for unmapped/low-fullbright pixels.

Images trace every pixel independently; statistics and CSVs use the subsampled grid only.
All outputs use exclusive creation. Preflight rejects existing named outputs and outputs under
the source-map directory, captures, game install or Steam workshop. Interrupted runs can leave
partial outputs; choose a new `--out` directory to rerun. Nothing is written to the input assets.

For actual textured renderer output from these same four poses, see
[render_pose's Baked recipe](../render_pose/README.md). Both tools share bake scene/material/caster
setup and instance collapse in `tools/common/bake_setup.*`; the render tool defaults to display
scale 1.0 and accepts `--bake-scale 2.02` to reproduce this probe's fitted set-2 multiplier.
