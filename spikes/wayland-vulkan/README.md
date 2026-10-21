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
- **Pointer grab, measured** (`--probe-grab`, no input needed): on `wayland`, Qt logs
  "This plugin supports grabbing the mouse only for popup windows" and
  `setMouseGrabEnabled(true)` returns false for both viewports even though they are
  active. On `xcb` (XWayland) it returns true for both. So Qt's public API cannot lock the
  pointer for a fly-cam on Wayland. The host compositor does advertise
  `zwp_pointer_constraints_v1` and `zwp_relative_pointer_manager_v1` (`wayland-info`), so
  a native integration (lock pointer + relative motion on the viewport's `wl_surface`) is
  possible; that is the route D13/D15 should assume.
- **Wayland, splitter, manual run (logs of 2026-10-20):**
  - Resize and splitter drags: both viewports track every step, swapchains are recreated
    each time, and presentation continues with no validation errors.
  - Pointer: enter/leave/move/press/release arrive with the correct viewport id. A right
    button press never gets an implicit grab across the two child windows: dragging from
    viewport 2 into viewport 1 sends viewport 2 a `mouse-leave` and delivers the release to
    viewport 1. The spike used to leave viewport 2 "captured" with a blank cursor; a
    right-button release anywhere now ends all captures (`InputProbe`).
  - Keyboard: **keys never reach the embedded `QWindow`.** `FocusIn` goes to the
    `QWindowContainer`, and key events go `QWidgetWindow` -> `QWindowContainer` ->
    `QSplitter` -> `QMainWindow`; the viewports log no `focus-in` or `key-` lines.
    Wayland keeps keyboard focus on the top-level surface. The containers therefore have to
    forward keys (`KeyForwarder`), which makes the shell, not the window, the input router.
    Verified on Wayland splitter: with the forwarder plus `setFocus` on the container when a
    viewport is clicked, `a`, held `w` (auto-repeat) and `b` reach the correct viewport. Without the
    click-to-focus, all keys stay with the first container. Right-drag capture (grab rejected,
    local-position-difference fallback) works and ends cleanly across viewports.
  - xcb (XWayland) splitter, measured: keys route correctly through the same forwarder
    (`a`, held `w` to viewport 1, `b` to viewport 2). Right-drag capture reports `grab=1`
    with `centre-warp`: 473 deltas summing to 567 px of continuous motion while the pointer
    stayed pinned at the viewport centre (x 249/250, y 300), so the warp fly-cam works under
    XWayland where Wayland rejects the grab.
  - Qt-ADS 4.4.1 dock on Wayland (`/tmp/wv-wayland-ads.log`): docked, keys route correctly
    (`a` to viewport 1, `b` to viewport 2). Then a dock rearrangement made viewport 2 resize to
    1000x574 and viewport 1 re-show at 490x562, and the compositor killed the connection:
    `wl_subsurface#156: error 0: no parent` / "The Wayland connection experienced a fatal error".
    The embedded viewports are wl_subsurfaces; ADS re-parents the container on float/redock/tab
    and Qt Wayland re-shows the child before its parent surface exists. Which gesture triggered it
    is still to be isolated.
  - ADS 4.4.1, floating enabled (`/tmp/wv-wayland-ads-float.log`): same `no parent` fatal error as
    soon as a dock was pulled out. With `--no-float` (`/tmp/wv-wayland-ads-nofloat.log`) the
    protocol error is avoided, but dragging a tab shows the drag-to-dock overlay and releasing
    never completes the drop; the UI stops responding while frames keep presenting.
  - Plain `QDockWidget` float/redock on Wayland (`/tmp/wv-wayland-qtdock*.log`): Qt recreates the
    native window and destroys the VkSurfaceKHR and its wl_display queue without delivering
    `SurfaceAboutToBeDestroyed` to the QWindow (only `SurfaceCreated` is ever seen). Validation:
    `VUID-vkDestroySurfaceKHR-surface-01266`. Destroying the swapchain afterwards segfaults in
    `wl_display_flush`; abandoning (leaking) it instead survives one float but the compositor
    then aborts the process ("Tried to add event to destroyed queue") when another dock is
    dragged. The float drag is also very jittery and docking targets are hard to hit.
    Conclusion: an embedded native Vulkan window cannot be reparented/floated safely on Wayland.
- Not yet done: ADS on xcb, HiDPI (all runs so far were dpr 1).
- ADS `ON` (4.4.1, needs `qt6-base-private-dev`): builds, and `--layout=dock --smoke`
  passes on `wayland` and `xcb`. Run it with
  `LD_LIBRARY_PATH=build/spike-wv-ads/_deps/qtads-build/x64/lib`.
