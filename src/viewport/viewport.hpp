#pragma once

// Viewport = Camera x DisplayMode x Overlays (plan 0.1, D15). "2D" is a preset of the same type.

#include "core/json.hpp"
#include "core/result.hpp"
#include "viewport/camera.hpp"

namespace apex::viewport {

enum class DisplayMode { Wireframe, Flat, Textured, Lit, Baked }; // Lit: 0.6, Baked: 0.7.

struct Overlays {
    bool grid = true;
    bool entityHelpers = true;
    bool bounds = false;
    bool normals = false;
    bool nodrawFaces = true; // False hides nodraw faces in Textured and Flat views.

    bool operator==(const Overlays&) const = default;
};

struct Viewport {
    Camera camera;
    DisplayMode mode = DisplayMode::Textured;
    Overlays overlays;
};

enum class Preset { Top, Front, Side, Camera };

// Top/Front/Side: Orthographic + AxisLocked + Wireframe + grid. Camera: Perspective + Free +
// Textured + grid. `focus` is the point the view centres on.
Viewport makePreset(Preset preset, DVec3 focus = DVec3{0.0}, double orthoHeight = 1024.0);
// Replaces projection/axis/mode/overlays with the preset's, keeping the focus point (the camera's
// pivot for perspective, its position for ortho).
void applyPreset(Viewport& viewport, Preset preset);

const char* nameOf(DisplayMode mode);
const char* nameOf(Preset preset);

json::Value toJson(const Viewport& viewport);
Result<Viewport> viewportFromJson(const json::Value& value);

} // namespace apex::viewport
