# 0.7.3 ray-query layer probe

2026-11-01, `build/linux-clang-release` (Clang, RelWithDebInfo), headless with
`QT_QPA_PLATFORM=offscreen`. No renderer, lighting or scene integration in this slice.

| Device | Ray query / acceleration structure exposed | Actually exercised |
| --- | --- | --- |
| llvmpipe (LLVM 21.1.8, 256 bits), Mesa 26.0.8-1ubuntu0.3, Vulkan 1.4 | Both extensions and feature bits true; deferred host operations present | Both enabled; compute queries, BLAS/TLAS builds and TLAS UPDATE passed |
| AMD RX 6600 (RADV), reported by the owner | Unverified from this environment | Not accessible: `/dev/dri` is absent; forcing `radeon_icd.json` yields `vkEnumeratePhysicalDevices failed with ERROR_INITIALIZATION_FAILED` |

`vulkaninfo` enumerated only llvmpipe. These timings are software Vulkan results, not RX 6600
hardware measurements. The existing Context selector prefers discrete GPUs; `APEX_VK_DEVICE`
overrides its case-insensitive name filter. No second Context or special test selector is needed.
On a host exposing the GPU, repeat the smoke/measurement commands below with
`APEX_VK_DEVICE='RX 6600'`; the tests print the selected device and validation status.

The smoke test passed all 55 assertions with validation enabled, including synchronization
validation (`VK_LAYER_VALIDATE_SYNC=1`): no recorded errors or warnings. It checks a quad hit,
miss, two translated instances with distinct distances/custom indices, instance masks, and old
and new positions after an in-place TLAS transform update. Invalid input rejection is also covered.

Optional-feature fallback was exercised with the installed `VK_LAYER_KHRONOS_profiles` layer:
hide all RT extensions, hide each of ray query / acceleration structure / deferred host operations
individually, or disable the rayQuery feature while retaining the three extensions. In all five
configurations Context creation and buffer transfers passed, both DeviceInfo flags were false,
and the smoke test printed `Skipped: ray query is not enabled on this device`.
Profiles/settings used for these probes are temporary files under `/tmp/apex-no-rt` and
`/tmp/apex-rayquery-profiles`; no runtime dependency or test configuration was added to Apex.

Measurement: 4,096 instances of one triangle BLAS in a 64x64 grid, one warmup followed by seven
samples. Wall-clock milliseconds include instance packing/upload/flush, size queries, AS and
scratch allocation, command recording/submission and fence wait; exclude BLAS construction and
TLAS destruction. UPDATE moves one instance and keeps the TLAS handle. These are synchronous API
costs, not GPU timestamp measurements.

| Validation | Build median (min–max), ms | UPDATE median (min–max), ms |
| --- | --- | --- |
| Off | 15.203 (12.367–16.049) | 14.382 (12.091–16.582) |
| On | 15.619 (15.045–18.251) | 15.155 (13.446–16.815) |

Reproduction:

```sh
cmake --build build/linux-clang-release -j3
QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux-clang-release --output-on-failure -j4
QT_QPA_PLATFORM=offscreen APEX_VK_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1 build/linux-clang-release/tests/apex_tests --test-case='render: ray query*'
QT_QPA_PLATFORM=offscreen APEX_VK_VALIDATION=0 APEX_MEASURE=1 build/linux-clang-release/tests/apex_measure_tests --test-case='ray query:*'
```

Full ctest: **981 tests, zero failures, five opt-in measurements skipped**, 36.16 seconds.
The existing cache has `FETCHCONTENT_FULLY_DISCONNECTED=ON`.
An initial smoke run caught an unintended SPIR-V `Int64` capability; the probe now uses the existing
`uvec2` buffer-address convention and needs no shaderInt64 feature. The corrected run and sync
validation run passed. RX 6600 validation and timing remain to be repeated on a host with GPU access.

## RX 6600 (RADV) run, 2026-11-01

Run outside Codex's sandbox with `APEX_VK_DEVICE="RX 6600"`. The smoke test passes with validation on (55 assertions, no messages).
4096 instances: TLAS build median 0.43 ms, update median 0.44 ms (7 samples). The Intel UHD 770 does not expose ray query and skips.

## D3 scene integration and corpus drag, 2026-11-01

`render::SceneAccel` consumes `DrawList::shadowCasters` and `previews` directly. There is
no second casting predicate. It expands instances in draw order, using that ordinal as
the custom index, and caches each BLAS by `(vertex buffer, firstIndex, indexCount)`.
The retained immutable positions/indices identify replaced uploads as well, preventing
stale cache reuse if a buffer handle is recycled. Rebuilds retain unchanged BLAS and
prune removed meshes. Empty scenes have no Vulkan TLAS and return CPU misses.

Both backends use the same float world transforms, including draw-local origins,
brush previews, model placements and nested-instance transforms. CPU geometry is
retained at the mirror's brush/model upload seam, including the posed model vertices;
neither backend reads buffers back. Embree is private to `lighting`; the render adapter
owns the Vulkan objects. CPU queries return distance, custom index and draw-relative
primitive ID. Vulkan exposes the TLAS for compute queries.

Transform-only `sync` returns `Updated`, retaining the Vulkan TLAS handle and all BLAS.
Embree retains the instance geometries and triangle scenes, recommits only changed
instance transforms, then commits its dynamic, LOW-quality top-level scene (Embree's
partial-update mode). Vulkan uses the existing in-place TLAS UPDATE. Topology changes
return `Rebuilt`; identical input returns `Unchanged`. Mutation requires exclusive CPU
access and completed prior GPU uses, matching the synchronous `Accel` contract.

The fixture cross-check traces 2,048 seeded random rays over the actual
`render_map.vmf` caster list, before a one-brush drag, after UPDATE and after rebuild.
It checks hit/miss, distance, custom index and primitive ID against a compute ray query,
and checks Embree occlusion against closest-hit results. Separate tests cover shared
BLAS ranges, nonzero origins, mirrored/scaled instances, clipped rays, empty scenes,
invalid input and the D1 brush/model exclusions. Existing D1 tests now assert scene
instance/triangle counts against the atlas caster list, including helper-model exclusion.

Focused tests passed with synchronization validation on llvmpipe (LLVM 21.1.8, 256 bits),
Vulkan 1.4: **13,945 assertions**, no validation errors or warnings. With the installed
Khronos profiles layer hiding the RT extensions, the same tests passed **6,296 assertions**;
the GPU comparisons skipped while the CPU builds, updates and queries still ran.
`APEX_VK_DEVICE='RX 6600'` found no matching device in this sandbox (`/dev/dri` is absent).
The RX 6600 smoke/timing result above predates this scene integration; this new
cross-check and corpus timing still need a hardware run.

Corpus selection scans all **510** installed P2CE SDK maps, expands fixed nested instances,
and ranks by total authored solids including repeated placements. The largest is
`sdk_content/maps/instances/p2editor/coop_exit.vmf`: **513** expanded source solids,
zero unresolved instances and zero mesh failures. Loading it with the real VFS,
materials and models produces **982 caster instances, 55,442 instanced triangles,
939 unique BLAS**. The measurement changes only the first caster instance's transform.

One warmup plus seven samples, validation off, wall-clock milliseconds. BLAS construction
is excluded: rebuild creates new top-level scenes using the warmed mesh cache; update
retains those scenes. Times include caster expansion, cache lookup/packing, Embree commits,
and (for the combined backend) Vulkan allocation/upload/submission/fence waits. Combined
times include Embree work and must not be interpreted as isolated Vulkan timings.

| Backend | Rebuild median (min–max), ms | Instance update median (min–max), ms |
| --- | --- | --- |
| Embree CPU | 1.498 (1.474–2.016) | 0.992 (0.968–1.137) |
| Embree + llvmpipe Vulkan | 9.855 (8.389–13.507) | 7.647 (7.508–9.211) |

These measurements fit a 16.7 ms drag frame on this corpus, but software Vulkan does not
establish the RX 6600 cost or the cost of a larger production map. Reproduce on the host:

```sh
cmake --build build/linux-clang-release -j3
python3 tools/check_layers.py
QT_QPA_PLATFORM=offscreen APEX_VK_DEVICE='RX 6600' APEX_VK_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1 build/linux-clang-release/tests/apex_tests --test-case='lighting: CPU acceleration*,render: scene acceleration*,mirror: shadow casters*,model mirror: only explicit*'
QT_QPA_PLATFORM=offscreen APEX_VK_DEVICE='RX 6600' APEX_VK_VALIDATION=0 APEX_MEASURE=1 build/linux-clang-release/tests/apex_measure_tests --test-case='scene acceleration:*'
QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux-clang-release --output-on-failure -j4
```

Sandbox build limitation: CMake regeneration attempted an existing zlib GitHub update and
failed with `Could not resolve host: github.com`. No dependency or FetchContent settings
were changed. Compilation/linking here used a temporary Ninja manifest copied from the
existing build graph, adding the registered sources and existing Embree static libraries.
All builds used `-j3`; no Embree translation units were rebuilt. A normal CMake regeneration
still needs to be checked on a host with dependency-fetch access.

Final verification against those rebuilt binaries: `tools/check_layers.py` passed with
zero violations; full offscreen ctest passed **985 tests, zero failures**, with six opt-in
measurements skipped, in **57.63 seconds**. The new corpus measurement was also run
explicitly as shown above. `git diff --check` passed.

## Scene acceleration on the RX 6600, 2026-11-01

Scene tests (Embree vs ray query agreement, caster-set equality, caching, update path) pass on RADV with validation on: 13 cases, 81915 assertions. Full suite 985 tests, no failures.
