#include "editor/block_tool.hpp"
#include "editor/editor_session.hpp"
#include "geometry/solid_block.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

namespace apex::editor {
namespace {
DVec3 point(const SurfacePlane& plane, DVec2 uv) {
    return plane.origin + plane.uAxis * uv.x + plane.vAxis * uv.y;
}
struct Corner {
    DVec2 uv;
    DVec3 projected;
    SnapKind kind;
};
std::optional<Corner> corner(EditorSession& session, Ray ray, const ToolView& view,
                             const SurfacePlane& plane, bool suspended) {
    const auto snapped = session.snap(ray, view, suspended, plane, false);
    if (snapped.kind == SnapKind::None) return {};
    const auto projected = snapped.point - plane.normal * glm::dot(snapped.point - plane.origin, plane.normal);
    const auto relative = projected - plane.origin;
    return Corner{{glm::dot(relative, plane.uAxis), glm::dot(relative, plane.vAxis)}, projected, snapped.kind};
}
const char* snapName(SnapKind kind) {
    switch (kind) {
    case SnapKind::Vertex: return "vertex";
    case SnapKind::EdgeMidpoint: return "midpoint";
    case SnapKind::Edge: return "edge";
    case SnapKind::Grid: return "grid";
    case SnapKind::Surface: return "surface";
    case SnapKind::None: return "";
    }
    return "";
}
std::string number(double value) {
    std::ostringstream text;
    text << value;
    return text.str();
}
// The perspective world grid fades out this far from the camera (render::GridSettings::fadeDistance).
constexpr double kWorldGridReach = 4096.0;
bool overWorldGrid(Ray ray, const SurfacePlane& plane) {
    const double denom = glm::dot(ray.dir, plane.normal);
    if (std::abs(denom) < 1e-9) return false;
    const double t = glm::dot(plane.origin - ray.origin, plane.normal) / denom;
    if (!std::isfinite(t) || t <= 0.0) return false;
    const auto offset = ray.at(t) - ray.origin;
    return glm::length(offset - plane.normal * glm::dot(offset, plane.normal)) <= kWorldGridReach;
}
}

SurfacePlane BlockTool::basePlane(const ToolView& view, bool* remembered) const {
    auto resolved = session_.workPlane().resolve();
    auto plane = resolved && session_.workPlane().pinned() ? resolved->plane : session_.fallbackPlane();
    if (remembered) *remembered = false;
    if (view.orthographic && !session_.workPlane().pinned() &&
        glm::dot(plane.normal, -view.cameraForward) < 1.0 - 1e-6) {
        const auto forward = glm::abs(view.cameraForward);
        const auto axis = forward.z >= forward.x && forward.z >= forward.y ? Axis::Z
            : forward.x >= forward.y ? Axis::X : Axis::Y;
        WorkPlaneState fallback;
        fallback.pinAxisPlane(axis, session_.fallbackPlane().origin);
        plane = fallback.resolve()->plane;
        if (glm::dot(plane.normal, view.cameraForward) > 0.0) {
            plane.normal = -plane.normal;
            plane.vAxis = -plane.vAxis;
        }
    }
    if (!view.orthographic && !session_.workPlane().pinned()) {
        if (const auto hit = session_.picks().first(view.cursorRay); hit && hit->side) {
            if (auto surface = planeFromFace(*session_.document(), hit->node, *hit->side)) return *surface;
        }
        // The world grid the viewport draws wins wherever the cursor is over it.
        if (lastPlane_ && !overWorldGrid(view.cursorRay, plane)) {
            plane = *lastPlane_;
            if (remembered) *remembered = true;
        }
    }
    return plane;
}

void BlockTool::rebuild(Gesture& g) {
    g.poly.reset();
    g.height = g.mouseHeight;
    if (!g.typed.empty()) {
        const auto parsed = std::from_chars(g.typed.data(), g.typed.data() + g.typed.size(), g.height);
        if (parsed.ec != std::errc{} || parsed.ptr != g.typed.data() + g.typed.size() || !std::isfinite(g.height)) return;
    }
    if (g.stage != Stage::Height) return;
    const auto lo = glm::min(g.start, g.end), hi = glm::max(g.start, g.end);
    if (lo.x == hi.x || lo.y == hi.y || g.height == 0.0) return;
    const double bottom = std::min(0.0, g.height), top = std::max(0.0, g.height);
    const auto& p = g.plane;
    const std::array<DVec3, 3> axes{p.uAxis, p.vAxis, p.normal};
    const DVec3 low{lo, bottom}, high{hi, top};
    std::vector<geometry::PlaneSide> sides;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        geometry::PlaneSide side;
        side.plane = {axes[i], glm::dot(axes[i], p.origin) + high[static_cast<int>(i)]};
        sides.push_back(side);
        side.plane = {-axes[i], -glm::dot(axes[i], p.origin) - low[static_cast<int>(i)]};
        sides.push_back(side);
    }
    if (auto poly = geometry::ConvexPolyhedron::fromPlanes(sides)) g.poly = std::move(*poly);
}
void BlockTool::updateGhosts() {
    std::vector<GhostSolid> ghosts;
    for (const auto& [source, g] : gestures_)
        if (g.poly) ghosts.push_back({*g.poly, session_.newSolidStyle().material});
    preview_.setGhosts(ghosts);
}
void BlockTool::publish() {
    updateGhosts();
    session_.notify();
}
void BlockTool::cancel() {
    if (gestures_.empty() && preview_.empty()) return;
    gestures_.clear();
    current_.reset();
    preview_.clear();
    session_.notify();
}
// Runs while a pane is being destroyed: never notify (observers may touch half-destroyed widgets).
// The viewport picks the preview up by revision on its next frame.
void BlockTool::forget(InputSource source) {
    if (current_ == source) current_.reset();
    hoverSuspended_.erase(source);
    if (gestures_.erase(source)) updateGhosts();
}
void BlockTool::drop(InputSource source) {
    if (!gestures_.contains(source)) return;
    forget(source);
    session_.notify();
}
bool BlockTool::owns(InputSource source) const {
    const auto it = gestures_.find(source);
    return it != gestures_.end() && (it->second.dragging || it->second.stage == Stage::Height);
}
bool BlockTool::commit(InputSource source) {
    auto it = gestures_.find(source);
    if (it == gestures_.end() || !it->second.poly || !session_.document()) return false;
    const auto block = geometry::solidBlock(*it->second.poly, session_.newSolidStyle());
    const double height = it->second.height;
    session_.edit("Create block", [&](document::Transaction& tx) {
        const auto handle = tx.create(block);
        if (session_.selection().mode() == SelectionMode::Faces) session_.selection().setMode(SelectionMode::Solids);
        const std::array<Target, 1> selected{session_.document()->get(handle)->id};
        session_.selection().replace(selected);
    });
    lastHeight_ = height;
    lastPlane_ = it->second.plane;
    drop(source);
    return true;
}
Handled BlockTool::input(const InputEvent& event, const ToolView& view) {
    const auto source = event.source;
    if (event.kind == InputKind::Leave) {
        const auto it = gestures_.find(source);
        if (it != gestures_.end() && it->second.stage == Stage::Base) drop(source);
        return true;
    }
    if (event.kind == InputKind::CancelPointer) return false;
    if (event.kind == InputKind::Cancel) {
        drop(source);
        return true;
    }
    if (!session_.document()) return false;
    current_ = source;
    auto it = gestures_.find(source);
    if (event.kind == InputKind::KeyDown && event.key == "Escape") {
        if (it != gestures_.end()) drop(source);
        else if (session_.workPlane().pinned()) return false;
        else if (lastPlane_) {
            lastPlane_.reset();
            hoveringLastPlane_ = false;
            publish();
        }
        return true;
    }
    if (event.kind == InputKind::Press && event.button == PointerButton::Right) {
        if (it == gestures_.end()) return false;
        if (it->second.stage == Stage::Height) {
            it->second = {};
            publish();
        } else drop(source);
        return true;
    }
    if (event.navigating) return false;
    if (event.kind == InputKind::Press && event.button == PointerButton::Left &&
        actionModifier(session_.preset(), event, "block.pick_plane")) {
        drop(source);
        if (const auto hit = session_.picks().first(view.cursorRay); hit && hit->side) {
            if (const auto plane = planeFromFace(*session_.document(), hit->node, *hit->side))
                session_.pinWorkPlane(*plane);
        } else session_.unpinWorkPlane();
        return true;
    }
    if (it == gestures_.end()) {
        hoverSuspended_[source] = transformModifiers(session_.preset(), event).suspend;
        basePlane(view, &hoveringLastPlane_);
        if (event.kind == InputKind::Move || event.modifierKey) publish();
    }
    if (it != gestures_.end() && it->second.stage == Stage::Height) {
        auto& g = it->second;
        if ((event.kind == InputKind::Press && event.button == PointerButton::Left) ||
            (event.kind == InputKind::KeyDown && event.key == "Enter")) {
            commit(source);
            return true;
        }
        if (event.kind == InputKind::KeyDown && !event.modifierKey) {
            const auto& key = event.key;
            if (key == "Backspace") {
                if (!g.typed.empty()) g.typed.pop_back();
            } else if (key.size() == 1 && ((key[0] >= '0' && key[0] <= '9') || key == "." || key == "-")) {
                if (key == "-") {
                    if (!g.typed.empty() && g.typed[0] == '-') g.typed.erase(0, 1);
                    else g.typed.insert(g.typed.begin(), '-');
                } else if (key != "." || g.typed.find('.') == std::string::npos) g.typed += key;
            } else return false;
        } else if (event.kind == InputKind::Move || event.modifierKey) {
            // Closest point on the extrusion line to the cursor ray; no surface picking.
            const auto centre = point(g.plane, (g.start + g.end) * 0.5);
            const auto n = g.plane.normal, d = view.cursorRay.dir;
            const auto w = view.cursorRay.origin - centre;
            const double a = glm::dot(d, d), b = glm::dot(d, n), denominator = a - b * b;
            std::optional<double> height;
            if (denominator > 1e-12) {
                const double t = (b * glm::dot(w, n) - glm::dot(w, d)) / denominator;
                height = glm::dot(w + d * std::max(0.0, t), n);
            } else if (view.orthographic && view.size.y > 0.0) {
                // Parallel rays have no unique closest point. Choose by vertical screen motion.
                height = g.initialHeight + (g.heightAnchor.y - event.cursor.y) * view.orthoHeight / view.size.y;
            }
            if (height) {
                g.mouseHeight = *height;
                if (!session_.snapSettings().suspended && !transformModifiers(session_.preset(), event).suspend)
                    g.mouseHeight = std::round(g.mouseHeight / session_.gridSize()) * session_.gridSize();
            }
        } else return false;
        rebuild(g);
        publish();
        return true;
    }
    const bool suspended = transformModifiers(session_.preset(), event).suspend;
    if (event.kind == InputKind::Press && event.button == PointerButton::Left) {
        const auto plane = basePlane(view);
        const auto start = corner(session_, view.cursorRay, view, plane, suspended);
        if (!start) return true;
        Gesture g;
        g.plane = plane;
        g.start = g.end = start->uv;
        g.cornerSnap = std::make_pair(start->projected, start->kind);
        g.dragging = true;
        g.mouseHeight = lastHeight_.value_or(session_.gridSize() * 8.0);
        gestures_[source] = std::move(g);
        publish();
        return true;
    }
    if (it == gestures_.end() || !it->second.dragging) return false;
    auto& g = it->second;
    const bool released = (event.kind == InputKind::Release && event.button == PointerButton::Left) ||
                          (event.kind == InputKind::Move && !event.left);
    if (event.kind == InputKind::Move || event.modifierKey || released) {
        if (const auto end = corner(session_, view.cursorRay, view, g.plane, suspended)) {
            g.end = end->uv;
            g.cornerSnap = std::make_pair(end->projected, end->kind);
        }
        if (event.shift) {
            const auto delta = g.end - g.start;
            const double side = std::max(std::abs(delta.x), std::abs(delta.y));
            g.end = g.start + DVec2{std::copysign(side, delta.x), std::copysign(side, delta.y)};
            g.mouseHeight = side;
            if (g.cornerSnap) g.cornerSnap->first = point(g.plane, g.end);
        }
        if (released) {
            if (g.start.x != g.end.x && g.start.y != g.end.y) {
                g.stage = Stage::Height;
                g.heightAnchor = event.cursor;
                g.initialHeight = g.mouseHeight;
            }
            g.dragging = false;
        }
        rebuild(g);
        publish();
        return true;
    }
    return false;
}
ToolOverlay BlockTool::overlay(const ToolView& view, InputSource source) const {
    ToolOverlay result;
    const auto it = gestures_.find(source);
    if (it == gestures_.end()) {
        if (!session_.document()) return result;
        const auto hover = hoverSuspended_.find(source);
        if (const auto start = corner(session_, view.cursorRay, view, basePlane(view),
                                      hover != hoverSuspended_.end() && hover->second)) {
            result.worldPoints.push_back({start->projected, session_.overlayPalette().block, 7.0f});
            result.labels.push_back({start->projected, snapName(start->kind), {8, -8}});
        }
        return result;
    }
    const auto& g = it->second;
    if (!g.dragging && g.stage == Stage::Base) return result;
    const std::array<DVec2, 4> corners{g.start, {g.end.x, g.start.y}, g.end, {g.start.x, g.end.y}};
    for (std::size_t i = 0; i < corners.size(); ++i)
        result.worldSegments.push_back({point(g.plane, corners[i]), point(g.plane, corners[(i + 1) % 4]),
                                       session_.overlayPalette().block, 2.0f});
    // Side lengths at the middle of the first u and v edges.
    for (std::size_t i = 0; i < 2; ++i) {
        const auto mid = (point(g.plane, corners[i]) + point(g.plane, corners[i + 1])) * 0.5;
        result.labels.push_back({mid, number(std::abs(i == 0 ? g.end.x - g.start.x : g.end.y - g.start.y)), {8, 8}});
    }
    if (g.dragging && g.cornerSnap) {
        result.worldPoints.push_back({g.cornerSnap->first, session_.overlayPalette().block, 7.0f});
        result.labels.push_back({g.cornerSnap->first, snapName(g.cornerSnap->second), {8, -8}});
    }
    if (g.stage == Stage::Height)
        result.labels.push_back({point(g.plane, (g.start + g.end) * 0.5) + g.plane.normal * g.height,
                                 "Height: " + (g.typed.empty() ? number(g.height) : g.typed), {8, -8}});
    return result;
}
ToolReadout BlockTool::readout() const {
    const auto it = current_ ? gestures_.find(*current_) : gestures_.end();
    std::string status = it == gestures_.end() && hoveringLastPlane_ && lastPlane_ && !session_.workPlane().pinned()
        ? "Drag base (last plane; Esc clears)" : "Drag base";
    if (it != gestures_.end() && it->second.stage == Stage::Height) {
        const auto& g = it->second;
        status = "Height: " + (g.typed.empty() ? number(g.height) : g.typed);
    }
    return {{"Block"}, {}, status, false};
}

} // namespace apex::editor
