#include "editor/overlay_tool.hpp"

#include "core/strings.hpp"
#include "editor/editor_session.hpp"
#include "editor/entity_defaults.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace apex::editor {
namespace {

// A handle within this many logical pixels of the cursor takes the press.
constexpr double kHandlePixels = 9.0;

std::optional<DVec3> onPlane(const Ray& ray, DVec3 origin, DVec3 normal) {
    const double denominator = glm::dot(ray.dir, normal);
    if (std::abs(denominator) < 1e-9) return std::nullopt;
    const double t = glm::dot(origin - ray.origin, normal) / denominator;
    if (!std::isfinite(t)) return std::nullopt;
    return ray.origin + ray.dir * t;
}

std::string classOf(const kv1::Node& block) {
    const auto* value = block.find("classname");
    return value ? value->decodedValue().value_or("") : "";
}

} // namespace

void OverlayTool::activate() { session_.selection().setMode(SelectionMode::Objects); }

void OverlayTool::setMode(Mode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    notice_.clear();
    session_.publishToolStatus();
}

bool OverlayTool::setParameter(std::string_view id, double value) {
    if (id != "mode") return false;
    setMode(value >= 0.5 ? Mode::Decal : Mode::Overlay);
    return true;
}

void OverlayTool::cancel() {
    if (drag_) dragTo({}, true);
    hovered_.reset();
}

void OverlayTool::documentChanged() {
    if (drag_ && !session_.document()) drag_.reset();
}

void OverlayTool::setNotice(std::string notice) {
    notice_ = std::move(notice);
    session_.publishToolStatus();
}

std::optional<OverlayTool::Target> OverlayTool::target() const {
    const auto* doc = session_.document();
    if (!doc) return std::nullopt;
    const auto active = session_.selection().active();
    const auto* id = active ? std::get_if<NodeId>(&*active) : nullptr;
    if (!id) return std::nullopt;
    const auto handle = doc->handle(*id);
    const auto* node = handle ? doc->get(*handle) : nullptr;
    if (!node || !node->block || document::overlayKind(*node->block) != document::OverlayKind::Overlay) return std::nullopt;
    auto quad = geometry::parseOverlay(*node->block);
    if (!quad) return std::nullopt;
    const auto* sides = node->block->find("sides");
    return Target{*id, *quad, geometry::parseSideList(sides ? sides->decodedValue().value_or("") : "")};
}

std::optional<OverlayHandle> OverlayTool::handleAt(const Target& target, const ToolView& view, DVec2 cursor) const {
    std::optional<OverlayHandle> best;
    double nearest = kHandlePixels;
    for (const auto& spot : overlayHandles(target.quad)) {
        const auto pixel = worldToPixel(view, spot.position);
        if (!pixel) continue;
        // Corners and scale handles win ties with the centre, so a small quad stays editable.
        const double distance = glm::length(*pixel - cursor) + (spot.handle.kind == OverlayHandleKind::Move ? 0.5 : 0.0);
        if (distance <= nearest) {
            nearest = distance;
            best = spot.handle;
        }
    }
    return best;
}

void OverlayTool::place(const document::OverlaySurface& face, DVec3 point) {
    const auto material = session_.newSolidStyle().material;
    if (material.empty()) return setNotice("Choose a material to place");
    kv1::Node block;
    std::string label;
    if (mode_ == Mode::Decal) {
        block = decalBlock(point, material);
        label = "Place decal";
    } else {
        const auto quad = placementQuad(face, point, session_.faceTextureSize(material));
        document::SurfaceLookup lookup(*session_.document());
        const auto surfaces = lookup.all();
        auto sides = footprintSides(surfaces, quad);
        if (std::ranges::find(sides, static_cast<int>(face.side)) == sides.end()) sides.insert(sides.begin(), static_cast<int>(face.side));
        block = overlayBlock(quad, material, sides);
        label = "Place overlay";
    }
    if (const auto* cls = session_.entityClasses_.find(classOf(block)))
        for (const auto& [key, value] : missingEntityDefaults(block, *cls)) block.append(kv1::Node::value(key, value)).value();
    session_.selection().setMode(SelectionMode::Objects);
    const auto done = session_.runEdit(label, EditorSession::AfterEdit::Computed, [&](document::Transaction& tx) -> Result<EditedObjects> {
        const auto handle = tx.create(std::move(block));
        EditedObjects result;
        result.items.push_back({session_.document()->get(handle)->id, EditedObjects::Item::Kind::Entity, {}, {}});
        return result;
    });
    if (!done) return setNotice(done.error().what());
    setNotice(mode_ == Mode::Decal ? "Placed decal (approximate preview)" : "Placed overlay");
}

void OverlayTool::writeQuad(NodeId node, const geometry::OverlayQuad& quad, const std::vector<int>& sides,
                            const std::string& label, const std::string& coalesceKey) {
    const auto done = session_.runEdit(label, EditorSession::AfterEdit::Keep, [&](document::Transaction& tx) -> Result<EditedObjects> {
        const auto handle = session_.document()->handle(node);
        if (!handle) return std::unexpected(Error{ErrorCode::InvalidArgument, "the overlay is gone"});
        for (const auto& [key, value] : quadKeys(quad)) tx.setKeyValue(*handle, key, value);
        tx.setKeyValue(*handle, "sides", geometry::sideListText(sides));
        return EditedObjects{};
    }, coalesceKey);
    if (!done) setNotice(done.error().what());
}

void OverlayTool::toggleFace(const Target& target, SideId side) {
    const auto sides = toggleSide(target.sides, static_cast<int>(side));
    const bool added = sides.size() > target.sides.size();
    writeQuad(target.node, target.quad, sides, added ? "Add overlay face" : "Remove overlay face", {});
    setNotice(added ? "Added face " + std::to_string(side) : "Removed face " + std::to_string(side));
}

void OverlayTool::dragTo(DVec3 point, bool finish) {
    if (!drag_) return;
    auto& drag = *drag_;
    if (finish) {
        // Escape: put the quad back (merging into the drag's own undo step, which then changes nothing).
        writeQuad(drag.node, drag.start, drag.startSides, "Edit overlay", drag.coalesceKey);
        drag_.reset();
        session_.publishToolStatus();
        return;
    }
    const auto quad = dragOverlayHandle(drag.start, drag.handle, drag.from, point);
    if (quad.problem()) return;
    // Listed faces stay while the quad still lands on them (a Shift-added face too); faces the new
    // footprint covers join.
    std::map<int, const document::OverlaySurface*> bySide;
    for (const auto& surface : drag.surfaces) bySide.emplace(static_cast<int>(surface.side), &surface);
    std::vector<int> sides;
    for (const int side : drag.startSides) {
        const auto found = bySide.find(side);
        if (found == bySide.end()) { sides.push_back(side); continue; } // Unresolved: not ours to drop.
        const auto fragment = document::clipToSurface(quad, *found->second);
        if (fragment && geometry::fragmentArea(*fragment) > kOverlayFootprintMinArea) sides.push_back(side);
    }
    for (const int side : footprintSides(drag.surfaces, quad))
        if (std::ranges::find(sides, side) == sides.end()) sides.push_back(side);
    writeQuad(drag.node, quad, sides, "Edit overlay", drag.coalesceKey);
}

Handled OverlayTool::input(const InputEvent& event, const ToolView& view) {
    if (event.kind == InputKind::Cancel || (event.kind == InputKind::KeyDown && event.key == "Escape")) {
        if (!drag_) return false;
        cancel();
        return true;
    }
    if (!session_.document() || event.navigating) return false;
    if (event.kind == InputKind::Leave) {
        if (hovered_) { hovered_.reset(); session_.notify(); }
        return false;
    }
    if (event.kind == InputKind::Move) {
        if (drag_) {
            if (const auto point = onPlane(view.cursorRay, drag_->start.origin, drag_->start.normal)) dragTo(*point, false);
            return true;
        }
        const auto current = target();
        const auto hovered = current ? handleAt(*current, view, event.cursor) : std::nullopt;
        if (hovered != hovered_) {
            hovered_ = hovered;
            session_.notify();
        }
        return false;
    }
    if (event.kind == InputKind::Release && event.button == PointerButton::Left) {
        if (!drag_) return false;
        drag_.reset();
        session_.publishToolStatus();
        return true;
    }
    if (event.kind != InputKind::Press || event.button != PointerButton::Left) return false;

    const auto current = target();
    if (current && !event.shift) {
        if (const auto handle = handleAt(*current, view, event.cursor)) {
            const auto from = onPlane(view.cursorRay, current->quad.origin, current->quad.normal);
            if (from) {
                document::SurfaceLookup lookup(*session_.document());
                drag_ = Drag{current->node, *handle, current->quad, current->sides, *from, lookup.all(),
                             "overlay-drag-" + std::to_string(++drags_)};
                hovered_ = handle;
                session_.notify();
                return true;
            }
        }
    }
    const auto hit = session_.picks().first(view.cursorRay);
    if (!hit) {
        setNotice("No face under cursor");
        return true;
    }
    // Clicking an overlay-like entity selects it rather than placing on top of it.
    if (!hit->side) {
        const auto handle = session_.document()->handle(hit->node);
        const auto* node = handle ? session_.document()->get(*handle) : nullptr;
        if (node && node->block && document::overlayKind(*node->block)) {
            session_.selection().setMode(SelectionMode::Objects);
            session_.selection().select(hit->node);
            setNotice({});
            return true;
        }
        setNotice("No face under cursor");
        return true;
    }
    if (event.shift) {
        if (!current) setNotice("Select an overlay to add or remove faces");
        else toggleFace(*current, *hit->side);
        return true;
    }
    document::SurfaceLookup lookup(*session_.document());
    const auto face = lookup.surface(*hit->side);
    if (!face) {
        setNotice("That face has no valid solid");
        return true;
    }
    place(*face, hit->point);
    return true;
}

ToolOverlay OverlayTool::overlay(const ToolView&, InputSource) const {
    ToolOverlay result;
    const auto current = target();
    if (!current) return result;
    const auto& palette = session_.overlayPalette();
    const auto& quad = current->quad;
    for (std::size_t i = 0; i < 4; ++i)
        result.worldSegments.push_back({quad.corner(i), quad.corner((i + 1) % 4), palette.vertexEdge, 2.0f});
    const auto active = drag_ ? std::optional<OverlayHandle>(drag_->handle) : hovered_;
    for (const auto& spot : overlayHandles(quad)) {
        const bool lit = active && *active == spot.handle;
        const Vec4 colour = lit ? palette.gizmoHighlight
                          : spot.handle.kind == OverlayHandleKind::Corner ? palette.vertexHandle
                          : spot.handle.kind == OverlayHandleKind::Rotate ? palette.gizmoZ : palette.gizmoScreen;
        result.worldPoints.push_back({spot.position, colour, lit ? 10.0f : 8.0f});
        if (spot.handle.kind == OverlayHandleKind::Rotate)
            result.worldSegments.push_back({quad.origin, spot.position, palette.gizmoZ, 1.0f});
    }
    return result;
}

ToolReadout OverlayTool::readout() const {
    ToolReadout readout;
    readout.badge = {"Overlay"};
    readout.detail = session_.newSolidStyle().material;
    readout.parameters.push_back({"mode", "Mode", ToolParameter::Kind::Choice, mode_ == Mode::Decal ? 1.0 : 0.0, 0.0, 1.0,
                                  {"Overlay", "Decal"}});
    std::string help = mode_ == Mode::Decal
        ? "Decal: click a face to place an infodecal. Approximate: the engine chooses its face at map load."
        : "Click a face to place; drag handles to move, rotate, scale or move a corner; Shift+click adds or removes a face.";
    readout.status = notice_.empty() ? help : notice_ + " · " + help;
    return readout;
}

} // namespace apex::editor
