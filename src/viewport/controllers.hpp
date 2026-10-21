#pragma once

// Camera controllers driven by plain input structs (no Qt). The UI fills a NavInput once per
// frame from its events and calls navigate().
//
// Mapping (matches Strata / Blender conventions):
//   perspective: RMB held = fly (mouse look + WASD, E/Q up/down, shift fast, ctrl slow)
//                MMB drag = orbit about the pivot, shift+MMB = pan, scroll = dolly
//   orthographic: MMB / RMB drag = pan, scroll = zoom about the cursor
// Axis-locked cameras never rotate.

#include "viewport/camera.hpp"

namespace apex::viewport {

struct NavInput {
    DVec2 cursor{0.0};       // Pixels, origin top-left (y down), inside the viewport.
    DVec2 cursorDelta{0.0};  // Pixels since the last call.
    DVec2 viewportSize{1280.0, 720.0};
    double scroll = 0.0;     // Wheel notches, positive = away from the user (zoom in).
    double dt = 0.0;         // Seconds since the last call.
    bool left = false, middle = false, right = false;
    bool shift = false, ctrl = false, alt = false;
    bool moveForward = false, moveBack = false, moveLeft = false, moveRight = false;
    bool moveUp = false, moveDown = false;
};

struct NavSettings {
    double lookDegreesPerPixel = 0.25;
    double flySpeed = 512.0; // Units per second.
    double fastMultiplier = 4.0;
    double slowMultiplier = 0.25;
    double dollyFraction = 0.1;   // Distance fraction per scroll notch.
    double zoomBase = 0.85;       // Ortho height multiplier per scroll notch.
    double maxPitch = 89.0;
    double minOrthoHeight = 1.0;
    double maxOrthoHeight = 262144.0;
    double minOrbitDistance = 1.0;
};

// Each returns true when it changed the camera.
bool flyUpdate(Camera& camera, const NavInput& input, const NavSettings& settings = {});
bool orbitUpdate(Camera& camera, const NavInput& input, const NavSettings& settings = {});
bool pan3DUpdate(Camera& camera, const NavInput& input, const NavSettings& settings = {});
bool dollyUpdate(Camera& camera, const NavInput& input, const NavSettings& settings = {});
bool pan2DUpdate(Camera& camera, const NavInput& input, const NavSettings& settings = {});
bool zoom2DUpdate(Camera& camera, const NavInput& input, const NavSettings& settings = {});

// Dispatches by projection and buttons; sets camera.controller to the one that acted.
bool navigate(Camera& camera, const NavInput& input, const NavSettings& settings = {});

} // namespace apex::viewport
