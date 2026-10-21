#include "viewport/controllers.hpp"

#include <algorithm>

namespace apex::viewport {

namespace {

bool nonzero(DVec2 v) { return v.x != 0.0 || v.y != 0.0; }

void look(Camera& camera, DVec2 delta, const NavSettings& s) {
    camera.angles.y -= delta.x * s.lookDegreesPerPixel; // Mouse right turns right (yaw is CCW).
    camera.angles.x += delta.y * s.lookDegreesPerPixel; // Mouse down looks down (+pitch).
    camera.angles.x = std::clamp(camera.angles.x, -s.maxPitch, s.maxPitch);
    camera.angles.y = std::fmod(camera.angles.y, 360.0);
    if (camera.angles.y < 0.0) camera.angles.y += 360.0;
}

double viewHeightPx(const NavInput& in) { return std::max(in.viewportSize.y, 1.0); }

// World units per pixel at the pivot distance (perspective) or on the view plane (ortho).
double unitsPerPixel(const Camera& camera, const NavInput& in) {
    if (camera.isOrtho()) return camera.orthoHeight / viewHeightPx(in);
    return 2.0 * camera.orbitDistance * std::tan(glm::radians(camera.fovYDegrees) * 0.5) /
           viewHeightPx(in);
}

} // namespace

bool flyUpdate(Camera& camera, const NavInput& in, const NavSettings& s) {
    if (camera.isOrtho() || !in.right) return false;
    bool changed = false;
    if (!camera.axisLocked() && nonzero(in.cursorDelta)) {
        look(camera, in.cursorDelta, s);
        changed = true;
    }
    const double fwd = (in.moveForward ? 1.0 : 0.0) - (in.moveBack ? 1.0 : 0.0);
    const double rgt = (in.moveRight ? 1.0 : 0.0) - (in.moveLeft ? 1.0 : 0.0);
    const double upd = (in.moveUp ? 1.0 : 0.0) - (in.moveDown ? 1.0 : 0.0);
    if ((fwd != 0.0 || rgt != 0.0 || upd != 0.0) && in.dt > 0.0) {
        const auto basis = camera.basis();
        double speed = s.flySpeed;
        if (in.shift) speed *= s.fastMultiplier;
        if (in.ctrl) speed *= s.slowMultiplier;
        // Vertical motion is along world Z, like Source noclip's up/down.
        camera.position += (basis.forward * fwd + basis.right * rgt + DVec3{0, 0, upd}) *
                           (speed * in.dt);
        changed = true;
    }
    if (changed) camera.controller = ControllerKind::Fly;
    return changed;
}

bool orbitUpdate(Camera& camera, const NavInput& in, const NavSettings& s) {
    if (camera.isOrtho() || camera.axisLocked() || !in.middle || in.shift ||
        !nonzero(in.cursorDelta)) {
        return false;
    }
    const DVec3 pivot = camera.pivot();
    look(camera, in.cursorDelta, s);
    camera.position = pivot - camera.forward() * camera.orbitDistance;
    camera.controller = ControllerKind::Orbit;
    return true;
}

bool pan3DUpdate(Camera& camera, const NavInput& in, const NavSettings& s) {
    (void)s;
    if (camera.isOrtho() || !in.middle || !in.shift || !nonzero(in.cursorDelta)) return false;
    const auto basis = camera.basis();
    const double upp = unitsPerPixel(camera, in);
    camera.position += (-basis.right * in.cursorDelta.x + basis.up * in.cursorDelta.y) * upp;
    camera.controller = ControllerKind::Orbit;
    return true;
}

bool dollyUpdate(Camera& camera, const NavInput& in, const NavSettings& s) {
    if (camera.isOrtho() || in.scroll == 0.0) return false;
    const DVec3 pivot = camera.pivot();
    const double factor = std::max(1.0 - s.dollyFraction * in.scroll, 0.05);
    camera.orbitDistance = std::max(camera.orbitDistance * factor, s.minOrbitDistance);
    camera.position = pivot - camera.forward() * camera.orbitDistance;
    camera.controller = ControllerKind::Orbit;
    return true;
}

bool pan2DUpdate(Camera& camera, const NavInput& in, const NavSettings& s) {
    (void)s;
    if (!camera.isOrtho() || !(in.middle || in.right) || !nonzero(in.cursorDelta)) return false;
    const auto basis = camera.basis();
    const double upp = unitsPerPixel(camera, in);
    camera.position += (-basis.right * in.cursorDelta.x + basis.up * in.cursorDelta.y) * upp;
    camera.controller = ControllerKind::Pan2D;
    return true;
}

bool zoom2DUpdate(Camera& camera, const NavInput& in, const NavSettings& s) {
    if (!camera.isOrtho() || in.scroll == 0.0) return false;
    const double aspect = in.viewportSize.x / viewHeightPx(in);
    const auto basis = camera.basis();
    const DVec2 ndc{2.0 * in.cursor.x / std::max(in.viewportSize.x, 1.0) - 1.0,
                    2.0 * in.cursor.y / viewHeightPx(in) - 1.0};
    const auto offset = [&](const Camera& c) {
        const auto half = c.halfExtents(aspect);
        return basis.right * (ndc.x * half.x) - basis.up * (ndc.y * half.y);
    };
    const DVec3 before = offset(camera);
    camera.orthoHeight = std::clamp(camera.orthoHeight * std::pow(s.zoomBase, in.scroll),
                                    s.minOrthoHeight, s.maxOrthoHeight);
    camera.position += before - offset(camera); // Keep the world point under the cursor fixed.
    camera.controller = ControllerKind::Pan2D;
    return true;
}

bool navigate(Camera& camera, const NavInput& in, const NavSettings& s) {
    bool changed = false;
    if (camera.isOrtho()) {
        changed |= pan2DUpdate(camera, in, s);
        changed |= zoom2DUpdate(camera, in, s);
    } else {
        changed |= flyUpdate(camera, in, s);
        changed |= orbitUpdate(camera, in, s);
        changed |= pan3DUpdate(camera, in, s);
        changed |= dollyUpdate(camera, in, s);
    }
    return changed;
}

} // namespace apex::viewport
