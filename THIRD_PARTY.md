# Third-party dependencies

Every dependency must be listed here with its licence. `tools/check_licenses.py` fails if a
dependency named in `cmake/deps.cmake` is missing from the table, or if a listed licence is not on
the allowed list (plan 0.1, D4). GPL of any version is not allowed.

The row's first column is the exact name used in `deps.cmake` (CPM) or `find_package`.

| Name | Licence | How obtained | Used by |
|---|---|---|---|
| CPM.cmake | MIT | vendored `cmake/CPM.cmake` (v0.42.0) | build |
| doctest | MIT | CPM, pinned commit | tests |
| Qt6 | LGPL-3.0 | system package, dynamically linked | ui, app |
| Qt6 Multimedia | LGPL-3.0 | system Qt module, dynamically linked; Windows deployment via windeployqt | ui (sound preview) |
| FFmpeg | LGPL-2.1-or-later | Qt's Windows build, 7.1.2, deployed by windeployqt | Qt Multimedia Windows backend |
| glm | MIT | CPM, pinned commit | core |
| spdlog | MIT | CPM, pinned commit | core |
| sourcepp | MIT | CPM, pinned commit (v2026.9.11; vpkpp, steampp, vtfpp, toolpp, mdlpp) | assets |
| Vulkan-Headers | Apache-2.0 OR MIT | CPM, pinned commit (vulkan-sdk-1.4.357.0) | render (through volk and VMA) |
| volk | MIT | CPM, pinned commit (vulkan-sdk-1.4.357.0) | render |
| VulkanMemoryAllocator | MIT | CPM, pinned commit (v3.3.0) | render |
| wayland-client | MIT | optional system package, dynamically linked on Linux | ui (pointer lock) |
| wayland-scanner | MIT | optional system build tool, located through pkg-config | build (protocol glue) |
| wayland-protocols | MIT | vendored upstream pointer-constraints and relative-pointer unstable v1 XMLs in `third_party/wayland-protocols/`, copyright/licence headers retained | ui (generated protocol glue) |

The protocol XMLs are copied from the [upstream Wayland protocol definitions](https://gitlab.freedesktop.org/wayland/wayland-protocols),
`unstable/pointer-constraints/pointer-constraints-unstable-v1.xml` and
`unstable/relative-pointer/relative-pointer-unstable-v1.xml` (retrieved 2026-10-22).
They require no installed `wayland-protocols` package. `pkg-config` is used only as a build tool.

Qt Multimedia uses Qt's FFmpeg backend on Windows (`ffmpegmediaplugin.dll` and the five
FFmpeg DLLs); windeployqt also deploys `windowsmediaplugin.dll`. The reference Linux host's
FFmpeg plugin pulls in a GPL-configured FFmpeg, libx264/libx265 and libmp3lame. Linux archives
intentionally omit the `multimedia` plugin directory and those codec libraries, so sound preview
is unavailable by default. An LGPL-configured FFmpeg bundle remains an open alternative.
