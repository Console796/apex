#pragma once

// Viewport = Camera x DisplayMode x Overlays (plan 0.1, D15). "2D" is a preset of the same type.

#include "core/json.hpp"
#include "core/result.hpp"
#include "viewport/camera.hpp"

#include <optional>

namespace apex::viewport {

// Realtime (0.6, formerly "Lit"): live lights and shadows, no lightmaps. Baked: 0.7, Vrad: 0.7.5, Hybrid: 0.7.7.
enum class DisplayMode { Wireframe, Flat, Textured, Realtime, Baked, Vrad, Hybrid };
// The modes the menus list first; the rest sit under an "Advanced" submenu.
constexpr DisplayMode kCommonModes[] = {DisplayMode::Wireframe, DisplayMode::Textured, DisplayMode::Realtime,
                                        DisplayMode::Hybrid};
constexpr DisplayMode kAdvancedModes[] = {DisplayMode::Flat, DisplayMode::Baked, DisplayMode::Vrad};

struct Overlays {
    bool grid = true;
    bool entityHelpers = true;
    bool bounds = false;
    bool normals = false;
    bool nodrawFaces = true; // False hides nodraw faces in Textured and Flat views.
    bool entityNames = false; // Draws each point entity's targetname beside it.
    bool leak = true;         // The compile's leak path, when there is one (0.5 D6).
    bool portals = true;      // The compile's portals, when there are any.
    bool cordons = true;      // The map's cordon boxes (0.5 D7).
    bool backfaceCulling = true; // False draws the back of every face too (Textured, Flat, Realtime).
    bool toolFixedBrightness = false; // Realtime/Baked: tool textures drawn at a fixed brightness, not lit or baked.
    bool luxelDensity = false;   // Baked/Vrad/Hybrid: the lightmap lattice as a checker, one cell per luxel (0.7.5 D7).
    bool realtimeIndirect = false; // Realtime: ray-traced irradiance probes (approximation).
    bool approxBounce = true;    // Realtime: constant ambient fill plus screen-space AO, a stand-in for bounce light.

    bool operator==(const Overlays&) const = default;
};

struct Viewport {
    Camera camera;
    DisplayMode mode = DisplayMode::Textured;
    Overlays overlays;
    // Compare (0.7.5 D7): the source a swap switches to, for flipping one viewport between Realtime, Baked and
    // Vrad. Session state only; not saved.
    std::optional<DisplayMode> compare;
};

enum class Preset { Top, Front, Side, Camera };

// Top/Front/Side: Orthographic + AxisLocked + Wireframe + grid. Camera: Perspective + Free +
// Textured + grid. `focus` is the point the view centres on.
Viewport makePreset(Preset preset, DVec3 focus = DVec3{0.0}, double orthoHeight = 1024.0);
// Replaces projection/axis/mode/overlays with the preset's, keeping the focus point (the camera's
// pivot for perspective, its position for ortho).
void applyPreset(Viewport& viewport, Preset preset);
// Quick A/B (0.7.6): one action flips the viewport between the Realtime view with realtime indirect
// probes and the Baked view, using the 0.7.5 compare state the same way its menu swap does:
// after a flip, `compare` marks the source the next flip shows, so the pane title keeps naming
// which is showing. The camera, projection and every overlay are kept, including the probes flag,
// which only affects Realtime; in Baked it stays isolated and off. `compare` stays session state.
void toggleBakedCompare(Viewport& viewport);

const char* nameOf(DisplayMode mode);
const char* nameOf(Preset preset);

json::Value toJson(const Viewport& viewport);
Result<Viewport> viewportFromJson(const json::Value& value);

} // namespace apex::viewport
