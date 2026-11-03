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

`tools/vrad_image_metrics.py` computes masked per-pose and pooled metrics; it
requires independently supplied masks. The masks have not been implemented:
nonblack VRAD pixels cannot safely be used, because sky, emissive surfaces,
and other non-lightmapped pixels may also be nonblack. No parity, held-out,
convergence, frame-time or before/after tables are claimed yet. The renderer
still needs a reliable visible-surface validity pass and cache-key validation
for the requested repeatable end-to-end harness.
