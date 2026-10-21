#include "viewport/view_context.hpp"

#include <algorithm>

namespace apex::viewport {

std::optional<double> WorkPlane::intersect(const Ray& ray, bool allowBehind) const {
    const double denom = glm::dot(normal, ray.dir);
    // Machine precision only detects exact parallelism.
    if (std::abs(denom) <= std::numeric_limits<double>::epsilon()) return std::nullopt;
    const double t = glm::dot(normal, origin - ray.origin) / denom;
    if (t < 0.0 && !allowBehind) return std::nullopt;
    return t;
}

DVec3 WorkPlane::project(DVec3 point) const {
    return point - normal * glm::dot(normal, point - origin);
}

DVec2 WorkPlane::coordinates(DVec3 point) const {
    const DVec3 d = point - origin;
    return {glm::dot(d, uAxis), glm::dot(d, vAxis)};
}

double GridSettings::snapValue(double v) const {
    if (!snap || spacing <= 0.0) return v;
    return std::round(v / spacing) * spacing;
}

DVec3 GridSettings::snapPoint(DVec3 p) const {
    return {snapValue(p.x), snapValue(p.y), snapValue(p.z)};
}

WorkPlane makeWorkPlane(const Camera& camera, const std::optional<AABB>& selection,
                        double groundZ) {
    WorkPlane plane;
    if (!camera.axisLocked()) {
        plane.origin = DVec3{camera.position.x, camera.position.y, groundZ};
        return plane;
    }
    const auto basis = camera.basis();
    plane.normal = -basis.forward;
    plane.uAxis = basis.right;
    plane.vAxis = basis.up;
    plane.origin = selection && !selection->empty() ? selection->center() : camera.position;
    return plane;
}

Ray cursorRay(const Camera& camera, DVec2 cursor, DVec2 size) {
    const double w = std::max(size.x, 1.0), h = std::max(size.y, 1.0);
    const double nx = 2.0 * cursor.x / w - 1.0;
    const double ny = 2.0 * cursor.y / h - 1.0;
    const auto b = camera.basis();
    const auto half = camera.halfExtents(w / h);
    if (camera.isOrtho()) {
        return {camera.position + b.right * (nx * half.x) - b.up * (ny * half.y), b.forward};
    }
    return {camera.position,
            glm::normalize(b.forward + b.right * (nx * half.x) - b.up * (ny * half.y))};
}

ViewContext makeViewContext(const Viewport& viewport, DVec2 cursor, DVec2 size,
                            const GridSettings& grid, const std::optional<AABB>& selection) {
    ViewContext ctx;
    ctx.cursorRay = cursorRay(viewport.camera, cursor, size);
    ctx.projection = viewport.camera.projection;
    ctx.axis = viewport.camera.axis;
    ctx.viewDirection = viewport.camera.forward();
    ctx.workPlane = makeWorkPlane(viewport.camera, selection);
    ctx.grid = grid;
    ctx.cursor = cursor;
    ctx.viewportSize = size;
    return ctx;
}

} // namespace apex::viewport
