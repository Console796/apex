# 0.7.6 step 1 P0

2026-11-01, headless Linux, existing `build/linux-clang-release` binaries.

`Context` reports llvmpipe (LLVM 21.1.8, 256 bits), Vulkan 1.4,
`ray_query true`, `accel_struct true`, validation off. Both features are enabled by
its existing feature check. This is software Vulkan; no RX 6600 hardware result is
claimed, and no Windows VM was used.

Existing `measure::scene acceleration: measure largest expanded corpus map brush drag`:
510 maps, largest `sdk_content/maps/instances/p2editor/coop_exit.vmf`, 513 expanded
source solids, zero unresolved instances or mesh failures. 982 caster instances,
55,442 triangles, 939 unique BLAS. One warmup and seven samples, synchronous wall
clock, excluding BLAS construction; transform-only drag retains the TLAS handle.

| Backend | Rebuild median (range), ms | Update median (range), ms |
| --- | --- | --- |
| Embree | 1.539 (1.471–3.751) | 1.010 (0.922–1.153) |
| Embree + Vulkan | 9.858 (8.818–13.348) | 9.372 (8.158–13.639) |

Combined updates fit 16.7 ms in this run but leave little software-rendering budget.
They include Embree work, packing/upload, Vulkan submission and fence wait, not
isolated GPU timestamps. Real hardware performance remains a separate measurement.

```sh
QT_QPA_PLATFORM=offscreen APEX_P2CE_DIR='/home/taylorg/.steam/steam/steamapps/common/Portal 2 Community Edition' APEX_VK_VALIDATION=0 APEX_MEASURE=1 build/linux-clang-release/tests/apex_measure_tests --test-case='scene acceleration:*'
```

Raw log: `/tmp/apex-gi-p0.log`. 1 case, 1,081 assertions passed.

## Thin-wall analysis before implementation

A 16-unit wall can lie entirely between 64-unit probe centres. Trilinear weights
alone mix both rooms. For a probe on the wrong side, the first-hit mean distance
is shorter than probe-to-surface distance. Chebyshev visibility
`variance / (variance + (distance - mean)^2)`, cubed to suppress its conservative
upper bound, attenuates that probe. A surface-normal offset keeps the query in air.
Angular depth filtering and finite rays can still overestimate variance at edges;
visibility moments alone cannot guarantee no leaks for arbitrary placement. Invalid
(back-facing/inside-solid) probes must be excluded, and grid coverage needs a dark
fallback. Relocation and nested cascades remain later work. A GPU thin-wall test
will quantify step 1 rather than treating this analytic argument as proof.
