# VRAD parity tuning (0.7.6): preliminary, incomplete

The target is the original `test1.vmf` VRAD output. A separate experiment used
`tools/derive_vrad_lights.py` to change only the five `light_rt` classnames
to `light` and the five `light_rt_spot` classnames to `light_spot`, leaving all
other bytes intact. The output is outside the repository. This does not rewrite
instance VMFs. `lighting::lightsFromEntity` uses the same code path for each
class pair. No tuning or default changes have been made.

On the RX 6600, at 640x360 and exposure 1, no direct specular, original and
derived Apex p1 images are byte-identical for Lit with 128 probe frames and for
Baked with 16 passes, 2 bounces, 32 rays and scale 0.62. Lit with 64 probe
frames and Baked at the settings above are also byte-identical for p2, h1 and
h2 (the other set-2 poses in `bake_parity_set2_0_7_4.md`). Each run reports 17 lights. This is not a
per-field light-list assertion or proof that VRAD uses matching attenuation.

The derived map's `-final` run completed with 449 fragments, 266 mapped,
181 no-data and 2 unowned; VRAD reports 21 direct lights. Surprisingly the
original map's `-final` log also reports 21 direct lights, despite the older
report saying it omitted runtime lights. Inspecting HDR worldlight lump 54,
both BSPs have 21 100-byte records; the ten records at the converted runtime
lights' origins are byte-identical across the two BSPs. The prior claim that
P2CE VRAD omits those ten lights was wrong. The two complete p1 renders differ
by at most 3 out of 255 per channel (mean absolute RGBA difference 0.045);
this includes non-lightmapped pixels and is **not** a parity metric.

The kept original-map `-final` BSP was replayed with `--vrad-bsp`; its p1 PNG
was byte-identical to the initial capture. Loading and mapping took 0.071 s,
compared with 7.315 s for the original compile and mapping. The caller must
validate that a cached BSP matches the VMF, instances, assets and flags.

## Preliminary six-pose measurement

Original map, RX 6600, 640x360, exposure 1, direct specular disabled, cached
`vrad -final` at scale 1. `--vrad-mask` marks mapped, nonblack brush lightmap
pixels, excluding sky, tools, displacements, selfillum/refraction, transparent
footprints and sides whose materials are in the game's `lights.rad`. It is
**experimental**, not a fully validated classification pass: map-local RAD
files are not read and the material/occluder exclusions lack targeted tests.
For this map the log found 59 RAD materials and two matching sides, but no
matching atlas fragments. Do not interpret these figures as a parity gate.

The four test poses are the set-2 positions in `bake_parity_set2_0_7_4.md`;
the two held-out views are p1's position at pitch -42/yaw 60 and p2's position
at pitch 25/yaw 150. The 16-pass, two-bounce, 32-ray Baked baseline below uses
**scale 1**, matching VRAD, not the editor's existing 0.62 default. Lit uses
128 probe frames. The Baked trial uses display scale 1.35, selected from the
four test poses, without changing the default. Ratios and errors are computed
only on the provisional mask; RGB MAE/RMSE use normalized linear values or
0–255 sRGB values. Shadow/lit means are split at each pose's median reference
linear luminance. Coarse RMSE uses normalized masked Gaussian blur, sigma 8 px.

| Pose | Baked 1.0 ratio / linear RMSE / 8-bit MAE | Baked 1.35 ratio / linear RMSE / 8-bit MAE | Lit ratio / linear RMSE / 8-bit MAE |
|---|---|---|---|
| p1 | 0.730 / 0.0560 / 13.95 | 0.985 / 0.0382 / 5.96 | 0.502 / 0.1011 / 27.43 |
| p2 | 0.716 / 0.0289 / 8.58 | 0.965 / 0.0205 / 3.09 | 0.502 / 0.0509 / 15.24 |
| p3 | 0.732 / 0.0481 / 12.72 | 0.987 / 0.0354 / 5.53 | 0.516 / 0.0851 / 24.53 |
| p4 | 0.749 / 0.0538 / 13.25 | 1.010 / 0.0392 / 5.70 | 0.530 / 0.0982 / 26.11 |
| h1 held out | 0.734 / 0.0729 / 17.55 | 0.990 / 0.0453 / 6.99 | 0.520 / 0.1295 / 33.89 |
| h2 held out | 0.665 / 0.0311 / 8.62 | 0.898 / 0.0130 / 2.51 | 0.435 / 0.0533 / 15.39 |

For p1, Baked 1.0 shadow/lit ratios were 0.653/0.735, versus 0.881/0.991 at
1.35; its coarse RMSE fell from 0.0535 to 0.0331. For h2 the respective
shadow/lit ratios were 0.684/0.663 and 0.923/0.894, and coarse RMSE fell from
0.0298 to 0.0112. The trial is evidence of a scale discrepancy, not proof of
its cause or a safe default across scenes. The four-pose 16-pass trial took
7.23 s end-to-end including setup and rendering; this is not convergence time.

Lit `--time 20` measured total synchronous frame times of 8.18–10.96 ms across
these views after warm-up. On p1 the separately measured off and on runs were
7.215 and 8.989 ms/frame respectively (about 1.77 ms difference; not a paired
profile). Shadow-atlas overflow was 1–3 for all views except p4 (zero), so
some direct shadows are missing and confound the comparison.

### Exploratory sampling curves, not a convergence proof

Four test poses, same provisional mask and Baked scale 1.35; times are entire
processes including setup and four images, not interactive restart times:

| Baked passes per level | 1 | 4 | 8 | 16 | 32 |
|---|---:|---:|---:|---:|---:|
| pooled linear RMSE vs VRAD | 0.034041 | 0.034072 | 0.034062 | 0.034017 | 0.034025 |
| wall seconds | 1.27 | 2.39 | 3.79 | 7.23 | 12.51 |

The one-pass error is within 0.05% of the 32-pass error, but systematic error
dominates this metric; that does not establish that the indirect field or
individual luxels have converged, nor that 32 passes is a converged reference.
It does not justify changing the live progressive bake's settings.

| Lit probe frames per pose | 1 | 32 | 128 | 256 | 512 |
|---|---:|---:|---:|---:|---:|
| pooled linear RMSE vs VRAD | 0.112205 | 0.108041 | 0.085770 | 0.084691 | 0.082903 |
| wall seconds for four poses | 1.09 | 2.54 | 6.31 | 11.92 | 22.25 |

Against the 512-frame result, 128 frames is within 5% of its per-pose and
pooled VRAD RMSE. The 512-frame image itself has not been proved converged,
and proximity in VRAD error can conceal an evolving indirect field. These
whole-process timings cannot be used as a per-reset convergence time.

No fast-start, bounce/ray sweep, larger final renders, systematic bias fix or
default change is claimed.
The PNG metrics helper supports pooled aggregates and additional error columns;
the rendered files and JSON from these exploratory runs are in `/tmp/opencode`
and the ignored worktree build directory, respectively. A self-contained
render/cache/metric driver and automatic cache-key validation are still needed.
