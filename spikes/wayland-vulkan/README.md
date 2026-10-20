# Wayland / Vulkan spike

Standalone throwaway C++23 project; no root build integration. Requires CMake,
Ninja, a C++23 compiler, system Qt 6.10 Widgets/Gui development packages, Vulkan
headers/loader and an installed Vulkan ICD (Mesa lavapipe works). Validation is
enabled when `VK_LAYER_KHRONOS_validation` is available. The default build does
not fetch anything.

From the repository root:

```sh
cmake -S spikes/wayland-vulkan -B build/spike-wv -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DSPIKE_ADS=OFF
cmake --build build/spike-wv

QT_QPA_PLATFORM=wayland build/spike-wv/wayland-vulkan
QT_QPA_PLATFORM=xcb build/spike-wv/wayland-vulkan
QT_QPA_PLATFORM=wayland build/spike-wv/wayland-vulkan --layout=dock
QT_QPA_PLATFORM=xcb build/spike-wv/wayland-vulkan --layout=dock
```

`--layout=splitter` is the default. Both layouts embed two Vulkan `QWindow`s
through `QWidget::createWindowContainer`. One native `VkInstance` is adopted by
one shared `QVulkanInstance` using `setVkInstance()` / `create()`. Each viewport
owns a device, graphics/present queue and swapchain. Rendering is intentionally
serial: transfer clear, layout transitions, FIFO presentation, one submission
at a time, with present semaphores per swapchain image. Resize, scale changes,
out-of-date and suboptimal results trigger recreation; native surface teardown
releases its swapchain/device before Qt destroys the surface.

Every application/Qt event is a flushed stdout line prefixed `viewport=1` or
`viewport=2`; `viewport=0` identifies application/Qt/validation diagnostics.
Logs include platform, validation availability, device/driver, pixel sizes,
screen/DPR, input and capture status. Key values are Qt key codes; mouse
coordinates/deltas are logical pixels. Present counts log at frame 1 and every
60 frames.

## Smoke

```sh
QT_QPA_PLATFORM=wayland build/spike-wv/wayland-vulkan --smoke
QT_QPA_PLATFORM=xcb build/spike-wv/wayland-vulkan --layout=dock --smoke

# Optional: force software Vulkan on this host.
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  QT_QPA_PLATFORM=xcb build/spike-wv/wayland-vulkan --smoke

# No desktop required, provided Xvfb can create sockets and lavapipe is installed.
xvfb-run -a env QT_QPA_PLATFORM=xcb \
  VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
  build/spike-wv/wayland-vulkan --smoke
```

Smoke resizes the main window at 700 ms and exits after approximately two
seconds. Exit 0 requires at least **10 successful/suboptimal presents in each
viewport** and no runtime error; otherwise it exits nonzero. These are successful
Vulkan presentation submissions, not compositor scanout measurements. An external
`timeout 15s` is useful for a stalled driver/compositor. Input and capture need
the manual checks below. `QT_QPA_PLATFORM=offscreen` cannot create the required
Vulkan surfaces and exits nonzero with an explanation.

## Pointer capture limitation to measure

Holding RMB hides the cursor and attempts `QWindow::setMouseGrabEnabled(true)`;
release, focus loss, hiding or native surface teardown restores it. On xcb,
centre warping produces continuing deltas. On Wayland, the spike logs differences
between local mouse positions and the grab result. In the upstream
[Qt 6.10.2 Wayland backend](https://github.com/qt/qtbase/blob/v6.10.2/src/plugins/platforms/wayland/qwaylandwindow.cpp#L1537),
this public grab API rejects non-popup windows. A rejected grab logs
`capture-limited ... unbounded-relative-motion=unavailable`; local deltas must
not be mistaken for a working fly camera lock. True unbounded Wayland motion
would require native relative-pointer / pointer-constraints integration, outside
this small public Qt API probe. No global cursor warping is attempted on Wayland.

## Optional ADS build

`SPIKE_ADS=ON` switches `--layout=dock` to Qt Advanced Docking System, pinned to
[tag 4.4.1](https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System/tree/4.4.1).
This is an opt-in FetchContent download and may require Qt Gui private development
headers and xcb development libraries. It is **best-effort and not build-verified
offline**; the default Qt dock path has no ADS dependency.

```sh
cmake -S spikes/wayland-vulkan -B build/spike-wv-ads -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DSPIKE_ADS=ON
cmake --build build/spike-wv-ads
QT_QPA_PLATFORM=wayland build/spike-wv-ads/wayland-vulkan --layout=dock
```

For an existing local checkout of that tag, also pass
`-DFETCHCONTENT_SOURCE_DIR_QTADS=/absolute/path/to/Qt-Advanced-Docking-System`.

## Manual checklist (repeat on wayland and xcb, splitter and dock)

- Confirm both animated colours, distinct viewport IDs, one shared instance log,
  device/driver names, and no validation errors.
- Drag splitter/dock boundaries, resize rapidly, minimize/restore. Check pixel
  resize and swapchain recreation logs and continued animation in both views.
- Move between monitors with different scales or change display scaling. Check
  show/screen/DPR logs and swapchain pixel sizes; confirm correct coverage.
- Click each viewport, type/hold/release keys, Tab between containers, and switch
  apps. Check focus in/out and that key logs identify the receiving viewport.
- Enter/leave each view; move, click and release each mouse button. Check event
  positions and IDs, including motion without a button held.
- Hold RMB, move across edges repeatedly, release. Check hidden/restored cursor,
  grab result, continuing signed deltas on xcb, and explicit Wayland limitations.
  Alt-Tab/hide during capture; confirm capture ends and cursor returns.
- Float, redock, resize, tab, close/reopen docks using the main window's dock
  context menu. Check continued rendering and native surface cleanup. Repeat
  with ADS when available (its docking UI differs).
- Run smoke for both layouts; require `smoke=pass`, both frame counts >= 10,
  exit 0, and no validation diagnostics. Exercise out-of-date via manual resize;
  the smoke resize alone does not guarantee the driver returns out-of-date.

## Results on this host (2026-10-20)

- System Qt 6.10.2, GCC 15.2, system Vulkan headers and loader: Debug build passes
  with `-Wall -Wextra`, no warnings.
- `--smoke` passes (exit 0, both viewports >= 10 presents, validation enabled, no
  diagnostics) for `QT_QPA_PLATFORM=wayland` and `xcb` (XWayland), each with
  `--layout=splitter` and `--layout=dock`, on RADV (AMD RX 6600, Mesa 26.0.8).
  Also passes on lavapipe/llvmpipe under XWayland and under `xvfb-run`.
- Not yet done: the manual checklist above (focus, keyboard routing, mouse, fly-cam
  capture, HiDPI, dock float/redock). The Wayland grab limitation is from reading Qt
  source, not yet measured. ADS `ON` is unverified.
