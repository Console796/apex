#include "render/detail_mirror.hpp"

#include "core/log.hpp"
#include "core/strings.hpp"
#include "document/displacement.hpp"
#include "geometry/solid_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace apex::render {
namespace {
using document::NodeId;

// One two-sided quad: a, b, c, d go round its edge; uv are the matching texture corners.
void appendQuad(DetailPlacement& out, const std::array<DVec3, 4>& corners, const std::array<Vec2, 4>& uv, DVec3 normal) {
    const auto first = static_cast<std::uint32_t>(out.vertices.size());
    for (std::size_t i = 0; i < 4; ++i) {
        const DVec3 p = corners[i] - out.origin;
        out.bounds.expand(p);
        out.vertices.push_back({{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)},
                                {static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)},
                                {uv[i].x, uv[i].y}, 0});
    }
    for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u, 0u, 2u, 1u, 0u, 3u, 2u}) out.indices.push_back(first + i);
}

// A sprite's rectangle in the plane through `origin` spanned by `across` and `up`: pos[0] is its
// upper-left corner, pos[1] its lower-right, in units scaled by the prop's scale.
void appendSprite(DetailPlacement& out, const geometry::DetailModel& model, DVec3 origin, DVec3 across, DVec3 up,
                  double scale) {
    const auto at = [&](Vec2 pos) { return origin + scale * (across * double(pos.x) + up * double(pos.y)); };
    const auto [ul, lr] = model.pos;
    const auto [tl, br] = model.tex;
    appendQuad(out, {at(ul), at({lr.x, ul.y}), at(lr), at({ul.x, lr.y})},
               {tl, Vec2{br.x, tl.y}, br, Vec2{tl.x, br.y}}, DVec3{0.0, 0.0, 1.0});
}

DVec3 rotateAbout(DVec3 v, DVec3 axis, double degrees) {
    const double r = glm::radians(degrees), c = std::cos(r), s = std::sin(r);
    return v * c + glm::cross(axis, v) * s + axis * glm::dot(axis, v) * (1.0 - c);
}

} // namespace

std::vector<DetailFace> detailFaces(const kv1::Node& solid) {
    std::vector<DetailFace> faces;
    const auto sides = solid.findAll("side");
    if (std::ranges::none_of(sides, [](const kv1::Node* side) { return document::isDisplaced(*side); })) return faces;
    const auto geometry = geometry::solidGeometry(solid);
    if (!geometry || !geometry->poly.isValid()) return faces;
    for (const auto* side : sides) {
        if (!document::isDisplaced(*side)) continue;
        const auto* id = side->find("id");
        if (!id) continue;
        const auto seed = id->asInt();
        auto disp = document::parseDisplacement(*side->find("dispinfo"));
        if (!seed || !disp) continue;
        const auto& poly = geometry->poly;
        const auto face = std::ranges::find(poly.faces, static_cast<std::uint32_t>(*seed), &geometry::Face::sideId);
        if (face == poly.faces.end()) continue;
        const auto corners = geometry::displacementCorners(poly, *face);
        if (!corners) continue;
        const auto* material = side->find("material");
        faces.push_back({*corners, std::move(*disp), static_cast<int>(*seed),
                         toLowerAscii(material ? material->decodedValue().value_or("") : "")});
    }
    return faces;
}

Result<void> appendDetailProps(const DetailFace& face, const geometry::DetailType& type, DetailPlacement& out) {
    const auto props = geometry::placeDetailProps(face.corners, face.disp, type, face.seed);
    if (!props) return std::unexpected(props.error());
    for (const auto& prop : *props) {
        const auto& model = type.groups[prop.group].models[prop.model];
        const DVec3 origin(prop.origin);
        const auto axes = angleVectors(DVec3(prop.angles));
        // Source's local +Y is left (document::entityPose); the sprite's x runs along it.
        const DVec3 left = -axes.right, up = axes.up;
        ++out.props;
        switch (model.type) {
        case geometry::DetailPropType::Model: {
            DMat4 transform{DMat3{axes.forward, left, up}};
            transform[3] = DVec4(origin, 1.0);
            out.models.push_back({model.model, transform});
            break;
        }
        case geometry::DetailPropType::Sprite:
            appendSprite(out, model, origin, left, up, prop.scale);
            break;
        case geometry::DetailPropType::ShapeCross:
            appendSprite(out, model, origin, left, up, prop.scale);
            appendSprite(out, model, origin, axes.forward, up, prop.scale);
            break;
        case geometry::DetailPropType::ShapeTri: {
            // Three quads round the up axis, pushed out by shape_size of the half width and leaning
            // out by shape_angle.
            const double half = 0.5 * std::abs(double(model.pos[1].x) - double(model.pos[0].x)) * prop.scale;
            for (int i = 0; i < 3; ++i) {
                const DVec3 across = rotateAbout(left, up, 120.0 * i);
                const DVec3 outward = glm::cross(across, up);
                const DVec3 lean = rotateAbout(up, across, -double(model.shapeAngle));
                appendSprite(out, model, origin + outward * (half * model.shapeSize / 255.0), across, lean, prop.scale);
            }
            break;
        }
        }
    }
    return {};
}

void DetailMirror::retire(State& state, std::vector<Buffer>& garbage) {
    if (state.vertices) garbage.push_back(std::move(state.vertices));
    if (state.indices) garbage.push_back(std::move(state.indices));
    state.indexCount = 0;
    state.models.clear();
    state.props = 0;
}

void DetailMirror::setVbsp(std::shared_ptr<const geometry::DetailVbsp> vbsp) {
    vbsp_ = std::move(vbsp);
    for (auto& [id, state] : solids_) state.stale = true;
}

bool DetailMirror::apply(const document::Document& doc, const document::ChangeSet& changes, std::vector<Buffer>& garbage) {
    bool changed = false;
    const auto drop = [&](NodeId id) {
        const auto found = solids_.find(id);
        if (found == solids_.end()) return;
        changed = changed || found->second.indexCount > 0 || !found->second.models.empty();
        retire(found->second, garbage);
        solids_.erase(found);
    };
    const auto read = [&](Handle<document::Solid> handle) {
        const auto* node = doc.get(handle);
        if (!node || !node->block) return;
        ids_[handle] = node->id;
        auto faces = detailFaces(*node->block);
        if (faces.empty()) return drop(node->id);
        auto& state = solids_[node->id];
        state.faces = std::move(faces);
        state.stale = true;
    };
    for (const auto& handle : changes.created)
        if (const auto* solid = std::get_if<Handle<document::Solid>>(&handle)) read(*solid);
    for (const auto& modified : changes.modified)
        if (const auto* solid = std::get_if<Handle<document::Solid>>(&modified.handle)) read(*solid);
    for (const auto& handle : changes.destroyed) {
        const auto* solid = std::get_if<Handle<document::Solid>>(&handle);
        if (!solid) continue;
        if (const auto found = ids_.find(*solid); found != ids_.end()) {
            drop(found->second);
            ids_.erase(found);
        }
    }
    return changed;
}

bool DetailMirror::refresh(const DetailTypeOf& detailType, const Upload& upload, std::vector<Buffer>& garbage) {
    bool changed = false;
    for (auto& [id, state] : solids_) {
        if (!state.stale) continue;
        // Wait until every face's material has said whether it sets %detailtype.
        std::vector<std::pair<const DetailFace*, const geometry::DetailType*>> placed;
        bool waiting = false;
        for (const auto& face : state.faces) {
            const auto name = detailType(face.material);
            if (!name) { waiting = true; continue; }
            if (const auto* type = vbsp_ && !name->empty() ? vbsp_->find(*name) : nullptr) placed.emplace_back(&face, type);
        }
        if (waiting) continue;
        state.stale = false;
        const bool had = state.indexCount > 0 || !state.models.empty();
        retire(state, garbage);
        if (placed.empty()) {
            changed = changed || had;
            continue;
        }
        DetailPlacement out;
        out.origin = placed.front().first->corners[0];
        for (const auto& [face, type] : placed)
            if (auto done = appendDetailProps(*face, *type, out); !done)
                log::warn("detail props: solid {} side {}: {}", id, face->seed, done.error().what());
        state.origin = out.origin;
        state.bounds = out.bounds;
        state.models = std::move(out.models);
        state.props = out.props;
        if (!out.indices.empty()) {
            auto vertices = upload(std::as_bytes(std::span{out.vertices}), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            auto indices = upload(std::as_bytes(std::span{out.indices}), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            if (vertices && indices) {
                state.vertices = std::move(*vertices);
                state.indices = std::move(*indices);
                state.indexCount = static_cast<std::uint32_t>(out.indices.size());
            } else {
                log::error("detail props: upload failed for solid {}", id);
                if (vertices) garbage.push_back(std::move(*vertices));
                if (indices) garbage.push_back(std::move(*indices));
            }
        }
        changed = true;
    }
    return changed;
}

void DetailMirror::draw(std::vector<MeshDraw>& meshes, const DrawHooks& hooks) const {
    for (const auto& [id, state] : solids_) {
        if (state.indexCount == 0) continue;
        const auto preview = hooks.place ? hooks.place(id, state.origin) : std::optional<std::uint32_t>(0);
        if (!preview) continue;
        MeshDraw draw;
        draw.vertices = &state.vertices;
        draw.indices = &state.indices;
        draw.indexCount = state.indexCount;
        draw.origin = state.origin;
        draw.previewIndex = *preview;
        draw.texture = hooks.slot;
        draw.alpha = hooks.alpha == AlphaMode::Opaque ? AlphaMode::Test : hooks.alpha;
        draw.highlight = hooks.highlight ? hooks.highlight(id) : Vec4(0.0f);
        draw.edgeOverlay = false;
        draw.bounds = state.bounds;
        meshes.push_back(draw);
    }
}

void DetailMirror::forEachModel(const std::function<void(NodeId, const DetailModelProp&)>& visit) const {
    for (const auto& [id, state] : solids_)
        for (const auto& model : state.models) visit(id, model);
}

void DetailMirror::clear(std::vector<Buffer>& garbage) {
    for (auto& [id, state] : solids_) retire(state, garbage);
    solids_.clear();
    ids_.clear();
}

std::size_t DetailMirror::propCount() const {
    std::size_t count = 0;
    for (const auto& [id, state] : solids_) count += state.props;
    return count;
}

bool DetailMirror::pending() const {
    return vbsp_ && std::ranges::any_of(solids_, [](const auto& entry) { return entry.second.stale; });
}

} // namespace apex::render
