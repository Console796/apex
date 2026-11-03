# VRAD parity tuning (0.7.6): Baked and Lit transport tuned; fast-start pending

The target is the original `test1.vmf` VRAD output. A separate experiment used
`tools/derive_vrad_lights.py` to change only the five `light_rt` classnames
to `light` and the five `light_rt_spot` classnames to `light_spot`, leaving all
other bytes intact. The output is outside the repository. This does not rewrite
instance VMFs. `lighting::lightsFromEntity` uses the same code path for each
class pair.

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
**scale 1**, matching VRAD, not the editor's former 0.62 default. Lit uses
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

No Lit fast-start, ray sweep or larger final renders are claimed.

## Baked decomposition and chosen defaults

The same six poses were rerun with VRAD `-final -bounce 0` and Baked bounces 0,
then VRAD `-final -bounce 2` and Baked bounces 2, all at scale 1. The expanded
test map has no `light_environment`, so this does not test sky/sun. The direct
VRAD run still includes texlights. Ratios below compare masked linear luminance;
the final column divides the mean per-pixel change from bounce 0 to bounce 2 in
Baked by the corresponding VRAD change, using the intersection of both masks.

| Pose | direct Baked/VRAD | 2-bounce total Baked/VRAD | two-bounce increment Baked/VRAD |
|---|---:|---:|---:|
| p1 | 1.122 | 1.081 | 1.049 |
| p2 | 1.093 | 1.067 | 1.046 |
| p3 | 1.094 | 1.073 | 1.054 |
| p4 | 1.108 | 1.077 | 1.049 |
| h1 held out | 1.088 | 1.074 | 1.063 |
| h2 held out | 1.070 | 1.044 | 1.028 |

Thus the earlier 0.72 ratio was primarily an **unequal bounce-depth** comparison,
not evidence for multiplying every term by 1.35: the default `-final` VRAD log
shows 28 `GatherLight` iterations on this map, while `-bounce 2` shows two.
Direct-only Apex is 7–12% brighter at equal display scale, and matched two-bounce
increments agree to within about 7%; differences remain, including texlight
scope, geometric mapping, materials and VRAD's gather algorithm. No direct or
bounce formula was changed on this evidence.

The fresh-install Baked defaults are now **2 → 8 bounces, 64 → 4 maximum passes
per level, and display scale 0.62 → 1.0** (32 bounce rays unchanged). The old
0.62 fit targeted game screenshots; direct-only and matched-bounce results here
show that retaining it would introduce a large display mismatch against VRAD.
Saved QSettings values are respected. The VRAD view is unchanged; render_pose's
previous implicit VRAD fallback remains 0.62 unless a scale is supplied.

| Pose | old default ratio / linear RMSE | new default ratio / linear RMSE |
|---|---|---|
| p1 | 0.453 / 0.1108 | 1.010 / 0.0211 |
| p2 | 0.444 / 0.0572 | 0.997 / 0.0114 |
| p3 | 0.454 / 0.0979 | 1.006 / 0.0186 |
| p4 | 0.464 / 0.1125 | 1.010 / 0.0218 |
| h1 held out | 0.455 / 0.1466 | 1.013 / 0.0240 |
| h2 held out | 0.413 / 0.0528 | 0.975 / 0.0071 |

For the four test poses, eight-bounce pooled RMSE was 0.018578 at one pass and
0.018570 at 16 passes; the four-pass run's per-pose errors are the values above.
One/16 passes took 2.49/25.06 s including setup and four renders. The old
64-pass, two-bounce run took 27.42 s including six renders, and the new
four-pass, eight-bounce run took 7.02 s including six renders. These are
whole-process timings, not interactive invalidation timings; proximity in
reference error does not prove every luxel has converged. Provisional mask,
single-map and lack of a sky-specific test remain limitations.

## Lit: zero-overflow direct and long-history baseline

An opt-in `--shadow-tile-cap 256` fits all requested lights in the existing
atlas, at lower spatial shadow resolution. On all six poses `--time 20` reports
zero cluster and shadow overflow. With probes off, the direct-only Lit / VRAD
`-bounce 0` luminance ratios are 1.160, 1.128, 1.140, 1.148, 1.128 and 1.100
for p1–p4, h1 and h2, respectively.

At 2048 frames with that cap, the p1–p4 full Lit / VRAD `-final` ratios are
0.554, 0.557, 0.563 and 0.580. Using intersected reference masks and subtracting
the no-probe Lit and zero-bounce VRAD images, the Lit/VRAD indirect-increment
ratios are 0.298, 0.315, 0.288 and 0.298. Four independent 2048-frame resets
plus setup, captures and 10 extra timing frames took 51.47 s. Warm on frame
times were 4.99–6.57 ms. This is a baseline, not a fast-start measurement.
The direct / indirect subtraction operates on sRGB-decoded displayed images,
not the underlying HDR buffers, so clipping or unlit terms can affect it.

## Lit: cosine-normalized surface transport

Probe-update hit radiance is `energy * albedo / pi`, but its directional
integration had used the calibrated *direct-light* response `(c + c²)/2`.
Over a hemisphere that integrates to `5*pi/6`, losing one sixth of a uniform
Lambertian surface's energy at every transport step. The CPU baker uses
cosine-weighted transport instead. Probe updates now integrate surface-hit
radiance with cosine; rays that hit/miss to sky retain the previous sky
angular response and its existing `6/(5*pi)` normalization. This is a transport
correction, not a fitted gain. The probe-off and Baked/VRAD paths are unchanged.

At 2048 frames and the zero-overflow 256-pixel shadow cap, before/after:

| Pose | full ratio before → after | linear RMSE before → after | indirect increment ratio before → after |
|---|---|---|---|
| p1 | 0.554 → 0.638 | 0.0916 → 0.0762 | 0.298 → 0.418 |
| p2 | 0.557 → 0.644 | 0.0464 → 0.0386 | 0.315 → 0.440 |
| p3 | 0.562 → 0.638 | 0.0776 → 0.0651 | 0.288 → 0.400 |
| p4 | 0.579 → 0.661 | 0.0889 → 0.0732 | 0.298 → 0.419 |
| h1 held out | 0.561 → 0.649 | 0.1192 → 0.0974 | not measured → 0.439 |
| h2 held out | 0.502 → 0.591 | 0.0487 → 0.0413 | not measured → 0.424 |

The map has no sky emitter; retaining the original sky kernel makes the
split-sky p1 image byte-identical to the original cosine trial. Steady-state
total frame times for the six corrected views were 5.09–6.79 ms at the 256 cap;
this cap is for analysis and reduces shadow resolution. The synthetic probe
room rose from 0.0585 to 0.0720 mean linear luminance; thin-wall leakage
remained below 0.57% in the three tested phases. No goldens were changed.

Substantial indirect energy remains missing; this change does not establish
whether visibility weighting, locality, missing texlights or other transport
effects dominate the remaining discrepancy. Sky/sun reference parity remains
unmeasured. A 2048-frame p1 accumulation after reset took 14.16 seconds on the
RX 6600 in the instrumented tool (including its first frame, not setup).
The PNG metrics helper supports pooled aggregates and additional error columns;
the rendered files and JSON from these exploratory runs are in `/tmp/opencode`
and the ignored worktree build directory, respectively. A self-contained
render/cache/metric driver and automatic cache-key validation are still needed.
