#include "editor/clip_tool.hpp"
#include "editor/topology_ops.hpp"
#include "editor/editor_session.hpp"
#include "editor/selection_expand.hpp"
#include "geometry/face_matching.hpp"

#include <algorithm>

namespace apex::editor {
namespace {
geometry::FaceCorrespondence correspondence(const geometry::ConvexPolyhedron& poly) {
    std::map<std::size_t, std::uint32_t> result;
    for (std::size_t i = 0; i < poly.faces.size(); ++i)
        if (!poly.faces[i].isCap) result.emplace(i, poly.faces[i].sideId);
    return result;
}
void capAttributes(const geometry::ConvexPolyhedron& source, geometry::ConvexPolyhedron& piece) {
    for (auto& face : piece.faces) {
        if (!face.isCap) continue;
        const auto parent = std::ranges::max_element(source.faces, {}, [&](const auto& old) {
            return glm::dot(face.plane.normal, old.plane.normal);
        });
        static_cast<geometry::FaceAttributes&>(face) = *parent;
        face.sideId = 0;
    }
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
}

SnapResult ClipTool::snap(const InputEvent& event, const ToolView& view) const {
    std::optional<SurfacePlane> fallback;
    if (view.orthographic && !session_.workPlane().pinned() && !session_.picks().first(view.cursorRay)) {
        const auto normal = -glm::normalize(view.cameraForward);
        const auto u = glm::normalize(glm::cross(view.cameraForward, view.cameraUp));
        fallback = SurfacePlane{session_.fallbackPlane().origin, normal, u, glm::cross(normal, u), {}};
    }
    return session_.snap(view.cursorRay, view, transformModifiers(session_.preset(), event).suspend, fallback);
}

void ClipTool::rebuild(Gesture& g) {
    g.pieces.clear();
    g.warning.clear();
    if (g.points.size() < 2 || (g.points.size() < 3 && !g.viewDirection)) return;
    const auto third = g.points.size() == 3 ? g.points[2] : g.points[0] + *g.viewDirection;
    const auto cross = glm::cross(g.points[1] - g.points[0], third - g.points[0]);
    if (!(glm::length(cross) > 1e-9)) { g.warning = "Clip points must define a plane"; return; }
    const auto plane = Plane::fromPoints(g.points[0], g.points[1], third);
    const auto selected = expandSelection(*session_.document(), session_.groups(), session_.selection());
    for (const auto node : selected.solidsAndEntities) {
        const auto handle = session_.document()->handle(node);
        if (!handle || !std::holds_alternative<Handle<document::Solid>>(*handle)) continue;
        const auto& block = *session_.document()->get(*handle)->block;
        if (hasDisplacement(block)) { g.warning = "Skipping displacement solids: topology is read-only"; continue; }
        const auto source = geometry::solidGeometry(block);
        if (!source) { g.warning = "Skipping invalid solids"; continue; }
        try {
            auto split = geometry::split(source->poly, plane);
            if (!split.front || !split.back) continue;
            capAttributes(source->poly, *split.front);
            capAttributes(source->poly, *split.back);
            g.pieces.push_back({std::get<Handle<document::Solid>>(*handle),
                                std::move(*split.front), std::move(*split.back)});
        } catch (const std::invalid_argument&) {
            g.warning = "Cannot clip: resulting geometry is degenerate";
            g.pieces.clear();
            return;
        }
    }
}
void ClipTool::updatePreview() {
    Preview::Replacements replacements;
    std::vector<GhostSolid> ghosts;
    for (const auto& [source, g] : gestures_) for (const auto& piece : g.pieces) {
        const auto* solid = session_.document()->get(piece.solid);
        if (!solid) continue;
        replacements.insert_or_assign(solid->id, keep_ == Keep::Back ? piece.back : piece.front);
        const auto& other = keep_ == Keep::Back ? piece.front : piece.back;
        ghosts.push_back({other, {}, keep_ == Keep::Both ? 0.55f : 0.2f,
                          keep_ == Keep::Both ? session_.overlayPalette().clip : session_.overlayPalette().clipDiscard});
    }
    preview_.setReplacements(replacements);
    preview_.setGhosts(ghosts);
}
void ClipTool::publish() {
    updatePreview();
    session_.notify();
}
bool ClipTool::perform(const ActionId& action) {
    if (action != "clip.cycle_keep" && action != id()) return false;
    keep_ = keep_ == Keep::Front ? Keep::Back : keep_ == Keep::Back ? Keep::Both : Keep::Front;
    for (auto& [source, g] : gestures_) rebuild(g);
    publish();
    return true;
}
void ClipTool::cancel() {
    if (gestures_.empty() && preview_.empty()) return;
    gestures_.clear();
    current_.reset();
    preview_.clear();
    session_.notify();
}
// Pane destruction must not notify observers of partially destroyed widgets.
void ClipTool::forget(InputSource source) {
    if (current_ == source) current_.reset();
    if (gestures_.erase(source)) updatePreview();
}
bool ClipTool::owns(InputSource source) const {
    const auto it = gestures_.find(source);
    return it != gestures_.end() && !it->second.points.empty();
}
void ClipTool::stepBack(InputSource source) {
    const auto it = gestures_.find(source);
    if (it == gestures_.end()) return;
    auto& g = it->second;
    g.dragging.reset();
    g.snapped.reset();
    if (!g.points.empty()) g.points.pop_back();
    if (g.points.empty()) forget(source);
    else rebuild(g);
    publish();
}
bool ClipTool::commit(InputSource source) {
    const auto it = gestures_.find(source);
    if (it == gestures_.end()) return false;
    rebuild(it->second); // Selection and document may have changed since the last pointer event.
    if (it->second.pieces.empty()) { publish(); return false; }
    session_.edit("Clip solids", [&](document::Transaction& tx) {
        for (const auto& piece : it->second.pieces) {
            // Copy from the intact source before the in-place rebuild removes any sides.
            if (keep_ == Keep::Both) tx.createFragment(piece.solid, piece.back);
            const auto& kept = keep_ == Keep::Back ? piece.back : piece.front;
            tx.rebuildSolid(piece.solid, kept, correspondence(kept));
        }
    });
    cancel();
    return true;
}
Handled ClipTool::input(const InputEvent& e, const ToolView& view) {
    if (e.kind == InputKind::Cancel) {
        if (owns(e.source)) { forget(e.source); publish(); }
        return true;
    }
    if (e.kind == InputKind::CancelPointer) return false;
    if (e.kind == InputKind::Leave) {
        if (const auto it = gestures_.find(e.source); it != gestures_.end()) it->second.dragging.reset();
        return false;
    }
    if (!session_.document()) return false;
    current_ = e.source;
    if ((e.kind == InputKind::KeyDown && e.key == "Escape") ||
        (e.kind == InputKind::Press && e.button == PointerButton::Right)) {
        if (!owns(e.source)) return false;
        stepBack(e.source);
        return true;
    }
    if (e.navigating) {
        if (const auto it = gestures_.find(e.source); it != gestures_.end()) it->second.dragging.reset();
        return false;
    }
    if (e.kind == InputKind::KeyDown && e.key == "Enter") { commit(e.source); return true; }
    const bool press = e.kind == InputKind::Press && e.button == PointerButton::Left;
    const bool release = e.kind == InputKind::Release && e.button == PointerButton::Left;
    auto it = gestures_.find(e.source);
    if (press) {
        auto& g = gestures_[e.source];
        g.dragging.reset();
        // Hit handles in screen space using the ray's pixel, just like feature snapping.
        const auto cursor = worldToPixel(view, view.cursorRay.at(1000.0));
        double best = 8.0;
        if (cursor) for (std::size_t i = 0; i < g.points.size(); ++i) {
            const auto pixel = worldToPixel(view, g.points[i]);
            if (pixel && glm::length(*pixel - *cursor) <= best) {
                best = glm::length(*pixel - *cursor);
                g.dragging = i;
            }
        }
        if (g.dragging) return true;
        if (g.points.size() == 3) return true;
        const auto snapped = snap(e, view);
        if (snapped.kind == SnapKind::None) return true;
        if (g.points.size() < 2) g.viewDirection = view.orthographic ? std::optional{view.cameraForward} : std::nullopt;
        g.points.push_back(snapped.point);
        g.snapped = snapped;
        rebuild(g);
        publish();
        return true;
    }
    if (it != gestures_.end() && it->second.dragging && (e.kind == InputKind::Move || release)) {
        auto& g = it->second;
        const auto snapped = snap(e, view);
        if (snapped.kind != SnapKind::None) {
            g.points[*g.dragging] = snapped.point;
            g.snapped = snapped;
            rebuild(g);
        }
        if (release || !e.left) g.dragging.reset();
        publish();
        return true;
    }
    return release && owns(e.source);
}
ToolOverlay ClipTool::overlay(const ToolView&, InputSource source) const {
    ToolOverlay result;
    const auto it = gestures_.find(source);
    if (it == gestures_.end()) return result;
    const auto& g = it->second;
    const Vec4 colour = session_.overlayPalette().clip;
    for (std::size_t i = 0; i < g.points.size(); ++i) {
        result.worldPoints.push_back({g.points[i], colour, 9.0f});
        result.labels.push_back({g.points[i], std::to_string(i + 1), {8, 8}});
        if (i) result.worldSegments.push_back({g.points[i - 1], g.points[i], colour, 2.0f});
    }
    if (g.points.size() == 3) result.worldSegments.push_back({g.points[2], g.points[0], colour, 2.0f});
    if (g.snapped) result.labels.push_back({g.snapped->point, snapName(g.snapped->kind), {8, -8}});
    return result;
}
ToolReadout ClipTool::readout() const {
    const auto it = current_ ? gestures_.find(*current_) : gestures_.end();
    const std::string keep = keep_ == Keep::Front ? "front" : keep_ == Keep::Back ? "back" : "both";
    if (it != gestures_.end() && !it->second.warning.empty()) return {{"Clip"}, {}, it->second.warning, true};
    return {{"Clip"}, {}, "Keep " + keep + "; place points, drag handles; Enter clips; Shift+X cycles", false};
}

} // namespace apex::editor
