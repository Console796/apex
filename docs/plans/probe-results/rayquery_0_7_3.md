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
cmake --build build/linux-clang-release -j
QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux-clang-release --output-on-failure -j8
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
