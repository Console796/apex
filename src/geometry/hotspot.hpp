#pragma once

// Plan 0.8 D13: hotspot texturing's pure fit. A material's hotspot rects (from its VTF's hotspot
// resource or a Hammer++ .rect file; assets/hotspot.hpp reads both) are regions of its texture;
// fitting picks the rect that best matches a face and maps the face onto it exactly.

#include "core/result.hpp"
#include "geometry/convex_polyhedron.hpp"

#include <cstdint>
#include <span>

namespace apex::geometry {

// Pixels in the texture, from the top left; min inclusive, max exclusive (Strata's wording:
// "pixel positions in the image starting from the top left").
struct HotspotRect {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool rotate = false;  // May be turned a random quarter turn.
    bool reflect = false; // May be flipped horizontally at random.
    bool alt = false;     // Alternate group: used only when asked for, and then exclusively.
    int width() const { return x2 - x1; }
    int height() const { return y2 - y1; }
    bool operator==(const HotspotRect&) const = default;
};

struct HotspotFitOptions {
    // World units per texel that a rect's pixel size is compared at: the editor's default texture
    // scale (Strata: "assumes that implementations will attempt to match rects with the default
    // texture scale as defined in the user configuration").
    double texelScale = 0.25;
    bool alternate = false; // Strata's left-Alt modifier: only alt-group rects.
    std::uint64_t seed = 0; // Chooses among equally good rects, turns and flips.
};

struct HotspotFit {
    FaceUv uv;
    std::size_t rect = 0; // Index into the rects passed.
    int quarterTurns = 0; // 0..3, counter-clockwise viewed from outside.
    bool reflected = false;
};

// Fits `points` (one face loop, in a plane with outward unit `normal`) to the best rect. The
// face's frame is its current U axis projected into the plane (V completes it, on the side of the
// current V axis), turned by quarter turns. Each candidate rect and turn is scored by how far the
// face's aspect is from the rect's (weighted twice) plus how far its texel size at `texelScale`
// is from the rect's, both as absolute log ratios. A rect without the rotate flag is still turned
// a quarter when that fits better (a trim strip runs up a pillar), but never at random. The seed
// picks among candidates within a small tolerance of the best: rects of the same size, a rotate
// rect's equivalent turns, and a reflect rect's flip. The face then fills the rect exactly
// (scale per axis, so a mismatched aspect stretches). Strata Hammer's own scoring is not
// published; this rule is ours.
Result<HotspotFit> fitHotspot(const FaceUv& current, DVec3 normal, std::span<const DVec3> points,
                              std::span<const HotspotRect> rects, const HotspotFitOptions& options);

} // namespace apex::geometry
