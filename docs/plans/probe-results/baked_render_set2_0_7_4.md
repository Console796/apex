# Baked renderer set 2 (0.7.4 slice 4a, 2026-11-01)

All four set-2 poses rendered offscreen using `apex_render_pose --mode baked`, on
`llvmpipe (LLVM 21.1.8, 256 bits)`, Vulkan 1.4. No GUI was launched. There is no `/dev/dri`
in this sandbox, but software Vulkan is usable. Validation layers were not enabled by the
release Context. The final rerender is pixel-identical to the initial run for all four poses.

Recipe and reproducible diff/region code: [tools/render_pose/README.md](../../../tools/render_pose/README.md).
Input: `~/maps/apex/test1.vmf`, P2CE game VFS plus addons, no supplied cubemap directory;
reference: `~/maps/apex/captures/set 2/N1.png`. Parameters: 1920x1080, Source FOV 90,
8 passes per level, 2 bounces, 32 bounce rays, 3 threads, seed 0, display scale 2.02,
exposure 1. Scale is user-selected, not a renderer default. Bake: 17 lights, 24 materials,
zero failed materials/textures. Renderer: 62 requested materials, zero failed materials/models.

Images, four signed-error PNGs and `comparison.txt` are in:

```
/tmp/claude-1000/-mnt-hdd-projects-apex/535ad622-6121-4838-a225-ecc2cfa4a9c0/scratchpad/baked/final/
```

Pose 11 region means use piecewise sRGB decoding and linear Rec.709 luminance, with the same
pixel rectangles in the editor/game images. Coordinates are left/top inclusive, right/bottom
exclusive. These avoid HUD/crosshair and compare complete surface appearance, including
reflections and texture detail, rather than the CPU probe's lit/fullbright irradiance estimate.

| Region | Pixel rectangle | Editor | Game | Editor/game |
|---|---|---:|---:|---:|
| floor | (400,900)–(1000,1040) | 0.025835 | 0.014683 | 1.7595 |
| ceiling | (400,20)–(800,140) | 0.165889 | 0.118201 | 1.4034 |
| shaded wall | (650,500)–(850,730) | 0.006634 | 0.005404 | 1.2275 |

The rendered regions are brighter than the game at this fitted scale. This is a visual tuning
baseline, not a parity acceptance claim. The game includes compiled BSP/VRAD transport and its
material/runtime terms; Baked uses VMF brush transport, retains envmaps/self-illumination, and
omits direct-light specular on baked brushes. Models/props/displacement sides retain Lit shading.

Verification: renderer-independent packer/UV/fragment/stale-layout/displacement-flag tests and
an offscreen quad test (HDR, scale/exposure, bilinear sampling, selected-face updates, missing
atlas) all ran on this machine, including GPU execution via llvmpipe. The existing CPU probe
also completed a no-assets smoke run (1 pass, 0 bounces, stride 128) through the shared helper;
its outputs are under `baked/probe-smoke/` and are not used for the numbers above.

Final gate: `cmake --build build/linux-clang-release -j3 && python3 tools/check_layers.py &&
QT_QPA_PLATFORM=offscreen ctest --test-dir build/linux-clang-release --output-on-failure -j4`
passed: 1006 discovered tests, zero failures, six opt-in measurement tests skipped, 57.05 seconds.
Focused atlas/Baked tests: six passed, 3951 assertions, including llvmpipe GPU execution.
