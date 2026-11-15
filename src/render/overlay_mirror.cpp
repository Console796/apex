#include "render/overlay_mirror.hpp"

#include "core/log.hpp"
#include "core/strings.hpp"

#include <algorithm>

namespace apex::render {
namespace {
using document::NodeId;

// The largest decal half-extent whose faces are kept for re-placing it once its size is known.
constexpr double kDecalReach = 1024.0;
// A solid edited this close to a decal's origin may change where the decal lands.
constexpr double kDecalWatch = 512.0;

bool hiddenNode(const document::Document& doc, const document::Node& node) {
    for (NodeId parent = node.parent; parent != 0;) {
        const auto handle = doc.handle(parent);
        const document::Node* p = handle ? doc.get(*handle) : nullptr;
        if (!p) return false;
        if (p->block && p->block->key() == "hidden") return true;
        parent = p->parent;
    }
    return false;
}

} // namespace

void OverlayMirror::retire(State& state, std::vector<Buffer>& garbage) {
    if (state.vertices) garbage.push_back(std::move(state.vertices));
    if (state.indices) garbage.push_back(std::move(state.indices));
    state.ranges.clear();
}

void OverlayMirror::build(NodeId id, State& state, const document::OverlayGeometry& geometry, const Upload& upload,
                          std::vector<Buffer>& garbage) {
    retire(state, garbage);
    state.material = toLowerAscii(geometry.material);
    state.unresolved = !geometry.missing.empty();
    state.solids.clear();
    if (geometry.problem) log::debug("overlay {}: {}", id, *geometry.problem);
    if (geometry.pieces.empty()) return;
    state.origin = geometry.quads.empty() ? geometry.pieces.front().fragment.vertices.front().position
                                          : geometry.quads.front().origin;
    // Group pieces by solid so each draw can follow its solid's preview transform.
    std::vector<const document::OverlayPiece*> pieces;
    for (const auto& piece : geometry.pieces) pieces.push_back(&piece);
    std::ranges::stable_sort(pieces, {}, [](const auto* piece) { return piece->solid; });
    std::vector<BrushVertex> vertices;
    std::vector<std::uint32_t> indices;
    for (const auto* piece : pieces) {
        state.solids.insert(piece->solid);
        if (state.ranges.empty() || state.ranges.back().solid != piece->solid)
            state.ranges.push_back({piece->solid, static_cast<std::uint32_t>(indices.size()), 0, {}});
        auto& range = state.ranges.back();
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const auto& fragment = piece->fragment;
        for (const auto& v : fragment.vertices) {
            const DVec3 local = v.position - state.origin;
            BrushVertex out{};
            out.position[0] = float(local.x);
            out.position[1] = float(local.y);
            out.position[2] = float(local.z);
            out.normal[0] = float(fragment.normal.x);
            out.normal[1] = float(fragment.normal.y);
            out.normal[2] = float(fragment.normal.z);
            out.uv[0] = float(v.uv.x);
            out.uv[1] = float(v.uv.y);
            out.material = 0;
            vertices.push_back(out);
            range.bounds.expand(local);
        }
        for (const auto& t : fragment.triangles)
            for (const auto index : t) indices.push_back(base + index);
        range.indexCount = static_cast<std::uint32_t>(indices.size()) - range.firstIndex;
    }
    auto vb = upload(std::as_bytes(std::span{vertices}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    auto ib = upload(std::as_bytes(std::span{indices}), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (!vb || !ib) {
        log::warn("overlay {}: upload failed", id);
        state.ranges.clear();
        return;
    }
    state.vertices = std::move(*vb);
    state.indices = std::move(*ib);
}

bool OverlayMirror::apply(const document::Document& doc, const document::ChangeSet& changes, const Upload& upload,
                          const DecalSize& decalSize, std::vector<Buffer>& garbage) {
    std::set<NodeId> changedSolids, rebuild;
    std::vector<AABB> changedBounds;
    bool solidCreated = false, hierarchy = false, changed = false;
    const auto solidChanged = [&](Handle<document::Solid> handle) {
        const auto* node = doc.get(handle);
        if (!node) return;
        changedSolids.insert(node->id);
        if (overlays_.empty() || !node->block) return;
        if (auto shape = geometry::solidGeometry(*node->block); shape && shape->poly.isValid())
            changedBounds.push_back(shape->poly.bounds());
    };
    const auto entityChanged = [&](Handle<document::Entity> handle) {
        const auto* node = doc.get(handle);
        if (!node || !node->block) return;
        if (document::overlayKind(*node->block)) {
            ids_[handle] = node->id;
            rebuild.insert(node->id);
        } else if (const auto found = overlays_.find(node->id); found != overlays_.end()) {
            retire(found->second, garbage); // No longer an overlay (classname edited).
            overlays_.erase(found);
            ids_.erase(handle);
            changed = true;
        }
    };
    for (const auto& handle : changes.created) {
        if (const auto* solid = std::get_if<Handle<document::Solid>>(&handle)) {
            solidCreated = true;
            solidChanged(*solid);
        } else if (const auto* entity = std::get_if<Handle<document::Entity>>(&handle)) entityChanged(*entity);
    }
    for (const auto& modified : changes.modified) {
        if (const auto* solid = std::get_if<Handle<document::Solid>>(&modified.handle)) solidChanged(*solid);
        else if (const auto* entity = std::get_if<Handle<document::Entity>>(&modified.handle)) entityChanged(*entity);
        if ((static_cast<unsigned>(modified.flags) & static_cast<unsigned>(document::Dirty::Hierarchy)) != 0) hierarchy = true;
    }
    for (const auto& handle : changes.destroyed) {
        const auto* entity = std::get_if<Handle<document::Entity>>(&handle);
        if (!entity) continue;
        const auto found = ids_.find(*entity);
        if (found == ids_.end()) continue;
        if (const auto state = overlays_.find(found->second); state != overlays_.end()) {
            retire(state->second, garbage);
            overlays_.erase(state);
            changed = true;
        }
        ids_.erase(found);
    }
    // Destroyed solids no longer resolve to IDs; any overlay that lies on a solid that is gone
    // rebuilds (its side list has already lost those sides, D11 rule 2).
    const bool solidDestroyed = std::ranges::any_of(changes.destroyed, [](const document::NodeHandle& handle) {
        return std::holds_alternative<Handle<document::Solid>>(handle);
    });
    for (auto& [id, state] : overlays_) {
        if (rebuild.contains(id)) continue;
        bool touched = std::ranges::any_of(state.solids, [&](NodeId solid) {
            return changedSolids.contains(solid) || (solidDestroyed && !doc.handle(solid));
        });
        if (state.unresolved && solidCreated) touched = true;
        if (state.decalOrigin) {
            const auto origin = *state.decalOrigin;
            for (const auto& bounds : changedBounds) {
                const DVec3 nearest = glm::clamp(origin, bounds.min, bounds.max);
                if (glm::length(nearest - origin) <= kDecalWatch) touched = true;
            }
            if (solidDestroyed) touched = true;
        }
        if (touched) rebuild.insert(id);
    }
    if (rebuild.empty() && !hierarchy) return changed;
    document::SurfaceLookup surfaces(doc);
    for (const auto id : rebuild) {
        const auto handle = doc.handle(id);
        const auto* node = handle ? doc.get(*handle) : nullptr;
        if (!node || !node->block) continue;
        auto& state = overlays_[id];
        state.hidden = hiddenNode(doc, *node);
        if (document::overlayKind(*node->block) == document::OverlayKind::Decal) {
            state.decal = node->block;
            state.candidates = document::decalCandidates(surfaces, *node->block, kDecalReach);
            const auto material = toLowerAscii(node->block->find("texture") ? node->block->find("texture")->decodedValue().value_or("") : "");
            state.decalSize = decalSize ? decalSize(material) : std::nullopt;
            state.decalOrigin.reset();
            if (const auto* origin = node->block->find("origin"))
                if (const auto parsed = origin->asVec3()) state.decalOrigin = DVec3{(*parsed)[0], (*parsed)[1], (*parsed)[2]};
            const auto geometry = document::decalGeometry(state.candidates, *node->block, state.decalSize.value_or(DVec2{64.0}));
            build(id, state, geometry, upload, garbage);
            for (const auto& surface : state.candidates) state.solids.insert(surface.solid);
        } else {
            build(id, state, document::overlayGeometry(surfaces, *node->block), upload, garbage);
        }
        changed = true;
    }
    if (hierarchy) {
        for (auto& [id, state] : overlays_) {
            const auto handle = doc.handle(id);
            const auto* node = handle ? doc.get(*handle) : nullptr;
            if (!node) continue;
            const bool hidden = hiddenNode(doc, *node);
            if (hidden != state.hidden) changed = true;
            state.hidden = hidden;
        }
    }
    return changed;
}

bool OverlayMirror::setPreview(const std::map<NodeId, document::OverlayGeometry>& previews, const Upload& upload,
                               std::vector<Buffer>& garbage) {
    bool changed = false;
    for (auto it = previews_.begin(); it != previews_.end();) {
        if (previews.contains(it->first)) { ++it; continue; }
        retire(it->second.state, garbage);
        it = previews_.erase(it);
        changed = true;
    }
    for (const auto& [id, geometry] : previews) {
        auto [it, inserted] = previews_.try_emplace(id);
        if (!inserted && it->second.geometry == geometry) continue;
        it->second.geometry = geometry;
        build(id, it->second.state, geometry, upload, garbage);
        changed = true;
    }
    return changed;
}

bool OverlayMirror::refreshDecals(const Upload& upload, const DecalSize& decalSize, std::vector<Buffer>& garbage) {
    if (!decalSize) return false;
    bool changed = false;
    for (auto& [id, state] : overlays_) {
        if (!state.decal) continue;
        const auto material = toLowerAscii(state.decal->find("texture") ? state.decal->find("texture")->decodedValue().value_or("") : "");
        const auto size = decalSize(material);
        if (!size || size == state.decalSize) continue;
        state.decalSize = size;
        const auto solids = state.solids;
        build(id, state, document::decalGeometry(state.candidates, *state.decal, *size), upload, garbage);
        state.solids.insert(solids.begin(), solids.end());
        changed = true;
    }
    return changed;
}

void OverlayMirror::draw(std::vector<MeshDraw>& meshes, const DrawHooks& hooks) const {
    for (const auto& [id, own] : overlays_) {
        const auto previewed = previews_.find(id);
        const State& state = previewed == previews_.end() ? own : previewed->second.state;
        if (own.hidden || state.ranges.empty()) continue;
        const auto material = hooks.material ? hooks.material(state.material) : Material{};
        const Vec4 tint = hooks.highlight ? hooks.highlight(id) : Vec4(0.0f);
        for (const auto& range : state.ranges) {
            const auto preview = hooks.place ? hooks.place(id, range.solid, state.origin) : std::optional<std::uint32_t>(0);
            if (!preview) continue;
            MeshDraw draw;
            draw.vertices = &state.vertices;
            draw.indices = &state.indices;
            draw.firstIndex = range.firstIndex;
            draw.indexCount = range.indexCount;
            draw.origin = state.origin;
            draw.previewIndex = *preview;
            draw.texture = material.slot;
            // An opaque overlay still draws on the alpha-test layer: its depth bias puts it in
            // front of the coplanar face it decorates.
            draw.alpha = material.alpha == AlphaMode::Opaque ? AlphaMode::Test : material.alpha;
            draw.highlight = tint;
            draw.edgeOverlay = false;
            draw.bounds = range.bounds;
            meshes.push_back(draw);
        }
    }
}

void OverlayMirror::clear(std::vector<Buffer>& garbage) {
    for (auto& [id, state] : overlays_) retire(state, garbage);
    for (auto& [id, previewed] : previews_) retire(previewed.state, garbage);
    overlays_.clear();
    previews_.clear();
    ids_.clear();
}

std::size_t OverlayMirror::pieceCount() const {
    std::size_t count = 0;
    for (const auto& [id, state] : overlays_) count += state.hidden ? 0 : state.ranges.size();
    return count;
}

std::set<std::string> OverlayMirror::materials() const {
    std::set<std::string> result;
    for (const auto& [id, state] : overlays_)
        if (!state.material.empty()) result.insert(state.material);
    return result;
}

} // namespace apex::render
