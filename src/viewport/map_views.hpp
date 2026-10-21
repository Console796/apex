#pragma once

// Per-map views (plan 0.1, D15/D19): Strata keeps up to four views in `viewsettings/views/v0..v3`
// with keys "3d" ("1" perspective, "0" 2D), "position" "(x y z)", "angle" "[p y r]" (3D only; the
// key is also accepted spelled "angles") and "zoom" (2D, pixels per world unit).
//
// Reading never mutates the file. Writing edits only values that actually changed (compared
// numerically), appending missing keys/blocks inside an existing `viewsettings` block, so an
// unchanged round trip is byte-identical. A VMF with no `viewsettings` block cannot be written
// (Unsupported): adding a top-level block would invalidate the typed document.

#include "core/result.hpp"
#include "document/vmf.hpp"
#include "viewport/layout.hpp"

#include <array>
#include <optional>

namespace apex::viewport {

struct MapView {
    bool is3d = true;
    DVec3 position{0.0};
    DVec3 angles{0.0};   // 3D only.
    double zoom = 1.0;   // 2D only: pixels per world unit.

    bool operator==(const MapView&) const = default;
};

using MapViews = std::array<std::optional<MapView>, 4>;

// Unparseable views are skipped (left nullopt) rather than failing the whole read.
MapViews readMapViews(const vmf::VmfFile& file);
Result<void> writeMapViews(vmf::VmfFile& file, const MapViews& views);

// 2D zoom (pixels per unit) <-> ortho height (world units visible vertically).
double orthoHeightFromZoom(double zoom, double viewportPixelHeight);
double zoomFromOrthoHeight(double orthoHeight, double viewportPixelHeight);

// Apply views to the first four panes (in layout order). 3D views set position/angles on a
// perspective pane (converting the pane to the Camera preset if needed); 2D views set the centre
// and zoom on an ortho pane (converting to Top/Front/Side by pane index if needed). The ortho
// depth coordinate is kept. Missing views leave their pane alone.
void applyMapViews(ViewportLayout& layout, const MapViews& views, double viewportPixelHeight = 768.0);
MapViews captureMapViews(const ViewportLayout& layout, double viewportPixelHeight = 768.0);

} // namespace apex::viewport
