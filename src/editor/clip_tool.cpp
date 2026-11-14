#include "editor/clip_tool.hpp"
#include "editor/topology_ops.hpp"
#include "document/displacement.hpp"
#include "editor/editor_session.hpp"
#include "editor/selection_expand.hpp"
#include "geometry/face_matching.hpp"
#include "geometry/tolerances.hpp"

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
// Whether piece still has the face of side `id` exactly as source had it (no cut, same corners).
bool wholeFace(const geometry::ConvexPolyhedron& source, const geometry::ConvexPolyhedron& piece, std::uint32_t id) {
    const auto before = std::ranges::find(source.faces, id, &geometry::Face::sideId);
    const auto after = std::ranges::find_if(piece.faces, [&](const geometry::Face& face) {
        return !face.isCap && face.sideId == id;
    });
    if (before == source.faces.end() || after == piece.faces.end() || before->vertices.size() != after->vertices.size())
        return false;
    return std::ranges::all_of(before->vertices, [&](std::size_t v) {
        return std::ranges::any_of(after->vertices, [&](std::size_t w) {
            return glm::distance(source.vertices[v], piece.vertices[w]) <= geometry::kPlanarEps;
        });
    });
}
}

std::vector<std::pair<NodeId, Handle<document::Solid>>> ClipTool::selectedSolids() const {
    std::vector<std::pair<NodeId, Handle<document::Solid>>> result;
    const auto selected = expandSelection(*session_.document(), session_.groups(), session_.selection());
    for (const auto node : selected.solidsAndEntities) {
        const auto handle = session_.document()->handle(node);
        if (handle && std::holds_alternative<Handle<document::Solid>>(*handle))
            result.emplace_back(node, std::get<Handle<document::Solid>>(*handle));
    }
    return result;
}

SnapResult ClipTool::snap(const InputEvent& event, const ToolView& view) const {
    // Only the selected solids are snap targets; other geometry is invisible to the cursor.
    std::set<NodeId> only;
    for (const auto& [node, solid] : selectedSolids()) only.insert(node);
    std::optional<SurfacePlane> fallback;
    if (view.orthographic && !session_.workPlane().pinned() &&
        !session_.picks().first(view.cursorRay, [&](NodeId node) { return only.contains(node); })) {
        const auto normal = -glm::normalize(view.cameraForward);
        const auto u = glm::normalize(glm::cross(view.cameraForward, view.cameraUp));
        fallback = SurfacePlane{session_.fallbackPlane().origin, normal, u, glm::cross(normal, u), {}};
    }
    return session_.snap(view.cursorRay, view, transformModifiers(session_.preset(), event).suspend,
                         fallback, true, {}, std::move(only));
}

std::vector<ClipTool::Source> ClipTool::clippableSources(std::string& warning) const {
    std::vector<Source> result;
    for (const auto& [node, handle] : selectedSolids()) {
        const auto& block = *session_.document()->get(handle)->block;
        const auto source = geometry::solidGeometry(block);
        if (!source) { warning = "Skipping invalid solids"; continue; }
        const auto displaced = document::displacedSides(block);
        result.push_back({handle, source->poly, {displaced.begin(), displaced.end()}});
    }
    return result;
}

// Orthographic gestures cut along the view direction and have no third point. Otherwise the cut
// is perpendicular to a face containing both points, so the third point sits above that face.
void ClipTool::inferThird(Gesture& g, std::span<const Source> sources) const {
    if (g.thirdManual && g.points.size() == 3) return;
    g.points.resize(std::min<std::size_t>(g.points.size(), 2));
    g.thirdManual = false;
    if (g.points.size() < 2 || g.viewDirection) return;
    const double length = glm::length(g.points[1] - g.points[0]);
    if (!(length > 1e-6)) return;
    constexpr double onFace = 0.01;
    const auto line = (g.points[1] - g.points[0]) / length;
    std::vector<const geometry::Face*> containing;
    for (const auto& source : sources) for (const auto& face : source.poly.faces)
        if (std::abs(face.plane.signedDistance(g.points[0])) <= onFace &&
            std::abs(face.plane.signedDistance(g.points[1])) <= onFace) containing.push_back(&face);
    // Along a shared edge several faces qualify: prefer the one most facing the camera, and skip any
    // whose perpendicular cut would just lie in another face (e.g. the line runs along a box edge).
    std::ranges::sort(containing, std::greater{}, [&](const geometry::Face* face) {
        return glm::dot(face->plane.normal, -g.cameraForward);
    });
    for (const auto* face : containing) {
        const auto cut = glm::cross(line, face->plane.normal);
        if (!(glm::length(cut) > 1e-6)) continue;
        const auto cutNormal = glm::normalize(cut);
        const bool inFace = std::ranges::any_of(containing, [&](const geometry::Face* other) {
            return std::abs(glm::dot(other->plane.normal, cutNormal)) > 1.0 - 1e-6;
        });
        if (inFace) continue;
        g.points.push_back((g.points[0] + g.points[1]) * 0.5 + face->plane.normal * (length * 0.5));
        return;
    }
}

void ClipTool::rebuild(Gesture& g) {
    g.pieces.clear();
    g.warning.clear();
    if (g.points.size() < 2) return;
    std::string skipped;
    const auto sources = clippableSources(skipped);
    inferThird(g, sources);
    if (g.points.size() < 3 && !g.viewDirection) return;
    if (g.points.size() == 2 && glm::length(g.points[1] - g.points[0]) < 1e-6) return; // Second point still being dragged out.
    const auto third = g.points.size() == 3 ? g.points[2] : g.points[0] + *g.viewDirection;
    const auto cross = glm::cross(g.points[1] - g.points[0], third - g.points[0]);
    if (!(glm::length(cross) > 1e-9)) { g.warning = "Clip points must define a plane"; return; }
    const auto plane = Plane::fromPoints(g.points[0], g.points[1], third);
    g.warning = skipped;
    for (const auto& source : sources) {
        try {
            auto split = geometry::split(source.poly, plane);
            if (!split.front || !split.back) continue;
            // D5: a clip may remove or reshape a displacement brush's flat sides, never its displaced
            // faces. The solid that stays in place keeps every displaced face whole (same side, same
            // corners), so its dispinfo stays valid unchanged.
            bool front = true, back = true;
            for (const auto id : source.displaced) {
                const bool inFront = wholeFace(source.poly, *split.front, id);
                const bool inBack = wholeFace(source.poly, *split.back, id);
                if (!inFront && !inBack) {
                    g.warning = "Cannot clip through a displaced face; Destroy displacement first";
                    g.pieces.clear();
                    return;
                }
                front = front && inFront;
                back = back && inBack;
            }
            if (!source.displaced.empty()) {
                const bool kept = keep_ == Keep::Front ? front : keep_ == Keep::Back ? back : front || back;
                if (!kept) {
                    g.warning = keep_ == Keep::Both ? "Cannot clip: displaced faces would end up in both pieces"
                                                    : "Cannot clip away a displaced face; keep its side or Destroy displacement first";
                    g.pieces.clear();
                    return;
                }
                // Keep both: the piece with the displaced faces is the one rebuilt in place.
                if (keep_ == Keep::Both && !front) std::swap(split.front, split.back);
            }
            capAttributes(source.poly, *split.front);
            capAttributes(source.poly, *split.back);
            g.pieces.push_back({source.solid, std::move(*split.front), std::move(*split.back)});
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
    if (g.points.size() == 3 && g.thirdManual) {
        g.thirdManual = false; // Back to the inferred third point, if there is one.
        g.points.pop_back();
    } else {
        if (g.points.size() > 1) g.points.resize(1);
        else g.points.clear();
        g.thirdManual = false;
    }
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
        g.cameraForward = view.cameraForward;
        if (g.dragging) return true;
        const auto snapped = snap(e, view);
        if (snapped.kind == SnapKind::None) return true;
        const bool lineDrawn = g.points.size() == 2 && glm::length(g.points[1] - g.points[0]) > 1e-6;
        if (lineDrawn && !g.viewDirection) {
            // No face to infer the cut plane from: the third point is placed by hand.
            g.points.push_back(snapped.point);
            g.thirdManual = true;
        } else {
            // Press anywhere else starts a new cut; the second point follows the pointer until release.
            if (g.points.size() >= 2) { g.points.clear(); g.thirdManual = false; }
            if (g.points.empty()) {
                g.viewDirection = view.orthographic ? std::optional{view.cameraForward} : std::nullopt;
                g.points.push_back(snapped.point);
            }
            g.points.push_back(snapped.point);
        }
        g.dragging = g.points.size() - 1;
        g.snapped = snapped;
        rebuild(g);
        publish();
        return true;
    }
    if (it != gestures_.end() && it->second.dragging && (e.kind == InputKind::Move || release)) {
        auto& g = it->second;
        g.cameraForward = view.cameraForward;
        const auto snapped = snap(e, view);
        if (snapped.kind != SnapKind::None) {
            g.points[*g.dragging] = snapped.point;
            if (*g.dragging == 2) g.thirdManual = true;
            g.snapped = snapped;
            rebuild(g);
        }
        if (release || !e.left) {
            // A click without a drag only places the first point; the next click places the second.
            if (*g.dragging == 1 && g.points.size() == 2 && glm::length(g.points[1] - g.points[0]) < 1e-6) {
                g.points.pop_back();
                rebuild(g);
            }
            g.dragging.reset();
        }
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
    const bool needsThird = it != gestures_.end() && it->second.points.size() == 2 && !it->second.viewDirection &&
        glm::length(it->second.points[1] - it->second.points[0]) > 1e-6;
    if (needsThird) return {{"Clip"}, {}, "Keep " + keep + "; click a third point to set the cut plane", false};
    return {{"Clip"}, {}, "Keep " + keep + "; drag to cut, drag handles to adjust; Enter clips; Shift+X cycles", false};
}

} // namespace apex::editor
