#pragma once

// Plan 0.8 D13: a material's hotspot rects. Strata documents two sources (wiki "Hotspot Embedded
// Format" and "Hotspot Text Format"): a VTF resource tagged "+\0\0", and Hammer++'s .rect
// KeyValues file, which a VMT names with %rectanglemap. The VTF resource wins (D13, as Strata
// Hammer); a .rect next to the material (materials/<material>.rect) is the last resort.

#include "assets/vfs.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace apex::assets {

struct HotspotRegion {
    static constexpr std::uint8_t kRotate = 0x1, kReflect = 0x2, kAlt = 0x4; // Strata's HotspotRectFlags_t.
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0; // Pixels from the top left.
    std::uint8_t flags = 0;
    bool operator==(const HotspotRegion&) const = default;
};

enum class HotspotSource { VtfResource, RectFile };

struct HotspotSet {
    HotspotSource source = HotspotSource::VtfResource;
    std::string path; // The VTF or .rect the rects came from.
    std::vector<HotspotRegion> rects;
};

// The hotspot resource of a VTF (7.3 or later), given at least the file's start; nullopt when the
// VTF has none. A resource that runs past `bytes` is an error, so a caller with a prefix can read
// the whole file and try again.
Result<std::optional<std::vector<HotspotRegion>>> readVtfHotspots(std::span<const std::byte> bytes);
// A .rect file: `Rectangles { rectangle { min "x y" max "x y" rotate 1 reflect 1 alt 1 } ... }`.
Result<std::vector<HotspotRegion>> parseRectFile(std::string_view text);
// The material's hotspots by D13's precedence: its $basetexture's resource, then its
// %rectanglemap, then materials/<material>.rect. nullopt when none has any rects.
Result<std::optional<HotspotSet>> loadHotspots(const Vfs& vfs, std::string_view material);

} // namespace apex::assets
