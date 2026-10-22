#include "editor/transform.hpp"

#include "core/strings.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace apex::editor {
namespace {

bool finite(DVec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }

double snapped(double value, double step) {
    if (!std::isfinite(step) || step <= 0.0) throw std::invalid_argument("invalid snap step");
    return std::round(value / step) * step;
}

Result<DVec3> vectorKey(const kv1::Node& block, std::string_view key) {
    const auto* value = block.find(key);
    if (!value) return DVec3{0.0};
    const auto parsed = value->asVec3();
    if (!parsed) return std::unexpected(parsed.error());
    return DVec3{(*parsed)[0], (*parsed)[1], (*parsed)[2]};
}

std::string vectorText(DVec3 value) {
    auto node = kv1::Node::value("", "");
    vmf::Value(&node).setGeometry(vmf::Vec3{value.x, value.y, value.z}).value();
    return node.decodedValue().value();
}

bool world(const kv1::Node& block) {
    const auto* classname = block.find("classname");
    return ciEquals(block.key(), "world") ||
           (classname && ciEquals(classname->decodedValue().value_or(""), "worldspawn"));
}

} // namespace

Transform::Transform(DMat3 linear, DVec3 translation) : linear_(linear), translation_(translation) {
    const double determinant = glm::determinant(linear);
    if (!finite(translation) || !finite(linear[0]) || !finite(linear[1]) || !finite(linear[2]) ||
        !std::isfinite(determinant) || determinant == 0.0 ||
        !finite(glm::inverse(linear)[0]) || !finite(glm::inverse(linear)[1]) || !finite(glm::inverse(linear)[2]))
        throw std::invalid_argument("transform must be finite and invertible");
}

Transform Transform::translate(DVec3 delta) { return {DMat3{1.0}, delta}; }
Transform Transform::rotate(DVec3 axis, double angleDeg, DVec3 pivot) {
    if (!finite(axis) || !std::isfinite(angleDeg) || glm::length(axis) == 0.0)
        throw std::invalid_argument("invalid rotation axis or angle");
    const DMat3 linear{glm::rotate(DMat4{1.0}, glm::radians(angleDeg), glm::normalize(axis))};
    return {linear, pivot - linear * pivot};
}
Transform Transform::scale(DVec3 factors, DMat3 frame, DVec3 pivot) {
    const Transform basis{frame, DVec3{0.0}};
    if (!basis.isRigid()) throw std::invalid_argument("scale frame must be orthonormal");
    DMat3 diagonal{1.0};
    for (int i = 0; i < 3; ++i) diagonal[i][i] = factors[i];
    const auto linear = frame * diagonal * glm::transpose(frame);
    return {linear, pivot - linear * pivot};
}
Transform Transform::mirror(DVec3 normal, DVec3 pivot) {
    if (!finite(normal) || glm::length(normal) == 0.0)
        throw std::invalid_argument("invalid mirror normal");
    normal = glm::normalize(normal);
    const auto linear = DMat3{1.0} - 2.0 * glm::outerProduct(normal, normal);
    return {linear, pivot - linear * pivot};
}
DMat4 Transform::matrix() const {
    DMat4 result{linear_};
    result[3] = DVec4{translation_, 1.0};
    return result;
}
Transform Transform::inverse() const {
    const auto linear = glm::inverse(linear_);
    return {linear, -linear * translation_};
}
bool Transform::isRigid() const {
    const auto gram = glm::transpose(linear_) * linear_;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(gram[i][j] - (i == j ? 1.0 : 0.0)) > 1e-10) return false;
    return true;
}
bool Transform::reversesOrientation() const { return glm::determinant(linear_) < 0.0; }
Transform Transform::operator*(const Transform& other) const {
    return {linear_ * other.linear_, point(other.translation_)};
}

DVec3 snapTranslate(DVec3 rawDelta, const AABB& bounds, DVec3 grab,
                    const SurfacePlane& plane, const SnapSettings& settings) {
    if (settings.suspended) return rawDelta;
    DVec3 corner = bounds.min;
    if (bounds.empty()) throw std::invalid_argument("empty snap bounds");
    for (int i = 0; i < 3; ++i)
        if (std::abs(bounds.max[i] - grab[i]) <= std::abs(bounds.min[i] - grab[i])) corner[i] = bounds.max[i];
    const auto anchor = settings.mode == SnapMode::Absolute ? corner - plane.origin : DVec3{0.0};
    auto result = rawDelta;
    for (const auto axis : {plane.uAxis, plane.vAxis}) {
        const double delta = glm::dot(rawDelta, axis), start = glm::dot(anchor, axis);
        result += axis * (snapped(start + delta, settings.gridSize) - start - delta);
    }
    return result;
}

double snapRotate(double rawDelta, double startingAngle, const SnapSettings& settings) {
    if (settings.suspended) return rawDelta;
    const double start = settings.mode == SnapMode::Absolute ? startingAngle : 0.0;
    return snapped(start + rawDelta, settings.fine ? 1.0 : settings.angleSnap) - start;
}

DVec3 snapScale(DVec3 rawFactors, const AABB& bounds, DVec3 pivot, const SnapSettings& settings) {
    if (settings.suspended) return rawFactors;
    if (bounds.empty()) throw std::invalid_argument("empty snap bounds");
    auto result = rawFactors;
    for (int i = 0; i < 3; ++i) {
        if (rawFactors[i] == 1.0) continue;
        const double endpoint = std::abs(bounds.min[i] - pivot[i]) > std::abs(bounds.max[i] - pivot[i])
                                    ? bounds.min[i] : bounds.max[i];
        const double reach = endpoint - pivot[i];
        if (reach == 0.0) continue;
        if (settings.mode == SnapMode::Absolute) {
            result[i] = (snapped(pivot[i] + reach * rawFactors[i], settings.gridSize) - pivot[i]) / reach;
        } else {
            const double size = bounds.max[i] - bounds.min[i];
            if (size != 0.0) result[i] = 1.0 + snapped(size * (rawFactors[i] - 1.0), settings.gridSize) / size;
        }
    }
    return result;
}

std::vector<DVec3> transformedVertices(const geometry::SolidGeometry& solid, const Transform& transform) {
    auto result = solid.poly.vertices;
    for (auto& vertex : result) vertex = transform.point(vertex);
    return result;
}

Result<geometry::SolidGeometry> applyToSolid(const geometry::SolidGeometry& solid,
                                            const Transform& transform, bool textureLock) {
    if (!solid.poly.isValid()) return std::unexpected(invalid("invalid source solid"));
    auto result = solid;
    result.poly.vertices = transformedVertices(solid, transform);
    const auto covector = glm::transpose(glm::inverse(transform.linear()));
    const auto validUv = [](const geometry::FaceUv& uv) {
        return finite(uv.uaxis) && finite(uv.vaxis) && std::isfinite(uv.shiftU) &&
               std::isfinite(uv.shiftV) && std::isfinite(uv.scaleU) && std::isfinite(uv.scaleV) &&
               uv.scaleU != 0.0 && uv.scaleV != 0.0;
    };
    const auto lock = [&](geometry::FaceUv& uv) {
        // Texture axes are covectors: dot(A^-T u, A p + t) + shift' = dot(u, p) + shift.
        uv.uaxis = covector * uv.uaxis;
        uv.vaxis = covector * uv.vaxis;
        uv.shiftU -= glm::dot(uv.uaxis, transform.translation()) / uv.scaleU;
        uv.shiftV -= glm::dot(uv.vaxis, transform.translation()) / uv.scaleV;
    };
    for (auto& face : result.poly.faces) {
        if (transform.reversesOrientation()) std::ranges::reverse(face.vertices);
        const auto plane = result.poly.derivePlane(face);
        if (!plane) return std::unexpected(invalid("transformed face is degenerate"));
        face.plane = *plane;
        if (!validUv(face.uv)) return std::unexpected(invalid("invalid source texture axes"));
        if (textureLock) lock(face.uv);
        if (!validUv(face.uv)) return std::unexpected(invalid("invalid transformed texture axes"));
    }
    // Keep source-side order, including redundant sides discarded by reconstruction.
    for (auto& side : result.sides) {
        const auto face = std::ranges::find(result.poly.faces, side.sideId, &geometry::Face::sideId);
        if (face != result.poly.faces.end()) {
            static_cast<geometry::FaceAttributes&>(side) = *face;
            side.plane = face->plane;
            side.points.clear();
            for (const auto index : face->vertices) side.points.push_back(result.poly.vertices[index]);
        } else {
            const auto normal = covector * side.plane.normal;
            const double length = glm::length(normal);
            side.plane = {normal / length, (side.plane.dist + glm::dot(normal, transform.translation())) / length};
            for (auto& point : side.points) point = transform.point(point);
            if (transform.reversesOrientation()) std::ranges::reverse(side.points);
            if (textureLock) lock(side.uv);
        }
    }
    const auto problems = result.poly.validate();
    if (!problems.empty()) return std::unexpected(invalid(problems.front().message));
    return result;
}

Result<EntityTransform> applyToEntity(const document::Document& doc, NodeId entity,
                                       const Transform& transform, bool textureLock) {
    const auto handle = doc.handle(entity);
    const auto* node = handle ? doc.get(*handle) : nullptr;
    if (!node || !std::holds_alternative<Handle<document::Entity>>(*handle) || world(*node->block))
        return std::unexpected(invalid("expected a non-world entity"));
    EntityTransform result;
    const auto walk = [&](const auto& self, NodeId id) -> Result<void> {
        const auto h = doc.handle(id);
        const auto* child = h ? doc.get(*h) : nullptr;
        if (!child) return std::unexpected(invalid("missing entity child"));
        if (std::holds_alternative<Handle<document::Solid>>(*h)) {
            const auto old = geometry::solidGeometry(*child->block);
            if (!old) return std::unexpected(old.error());
            auto solid = applyToSolid(*old, transform, textureLock);
            if (!solid) return std::unexpected(solid.error());
            result.solids.emplace(id, std::move(*solid));
        }
        for (const auto descendant : child->children) {
            const auto status = self(self, descendant);
            if (!status) return status;
        }
        return {};
    };
    const auto status = walk(walk, entity);
    if (!status) return std::unexpected(status.error());
    const bool point = result.solids.empty();
    if (point || node->block->find("origin")) {
        const auto origin = vectorKey(*node->block, "origin");
        if (!origin) return std::unexpected(origin.error());
        result.origin = transform.point(*origin);
    }
    if (point) {
        const auto angles = vectorKey(*node->block, "angles");
        if (!angles) return std::unexpected(angles.error());
        const auto basis = basisFromAngles(*angles);
        const auto forward = transform.linear() * basis[0], up = transform.linear() * basis[2];
        if (!finite(forward) || !finite(up)) return std::unexpected(invalid("invalid entity basis"));
        result.angles = anglesFromForward(forward, up);
    }
    return result;
}

Result<void> stageTransform(const document::Document& doc, document::Transaction& tx,
                            std::span<const NodeId> selection, const Transform& transform,
                            TransformOptions options) {
    std::set<NodeId> nodes;
    const auto walk = [&](const auto& self, NodeId id) -> bool {
        if (!nodes.insert(id).second) return true;
        const auto h = doc.handle(id);
        if (!h) return false;
        for (const auto child : doc.get(*h)->children) if (!self(self, child)) return false;
        return true;
    };
    for (const auto id : selection)
        if (!walk(walk, id)) return std::unexpected(invalid("missing transform selection node"));
    std::map<NodeId, geometry::SolidGeometry> solids;
    std::map<NodeId, EntityTransform> entities;
    for (const auto id : nodes) {
        const auto h = *doc.handle(id);
        const auto* node = doc.get(h);
        if (std::holds_alternative<Handle<document::Entity>>(h) && !world(*node->block)) {
            auto entity = applyToEntity(doc, id, transform, options.textureLock);
            if (!entity) return std::unexpected(entity.error());
            solids.merge(entity->solids);
            entities.emplace(id, std::move(*entity));
        }
    }
    for (const auto id : nodes) {
        const auto h = *doc.handle(id);
        if (!std::holds_alternative<Handle<document::Solid>>(h) || solids.contains(id)) continue;
        const auto old = geometry::solidGeometry(*doc.get(h)->block);
        if (!old) return std::unexpected(old.error());
        auto solid = applyToSolid(*old, transform, options.textureLock);
        if (!solid) return std::unexpected(solid.error());
        solids.emplace(id, std::move(*solid));
    }
    try {
        for (const auto& [id, solid] : solids)
            tx.setSolidGeometry(std::get<Handle<document::Solid>>(*doc.handle(id)), solid);
        for (const auto& [id, entity] : entities) {
            const auto h = *doc.handle(id);
            if (entity.origin) tx.setKeyValue(h, "origin", vectorText(*entity.origin));
            if (entity.angles) tx.setKeyValue(h, "angles", vectorText(*entity.angles));
        }
    } catch (const std::invalid_argument& error) {
        return std::unexpected(invalid(error.what()));
    }
    return {};
}

Result<void> commitTransform(document::Document& doc, std::span<const NodeId> selection,
                              const Transform& transform, TransformOptions options, std::string label) {
    try {
        auto tx = doc.begin(std::move(label));
        const auto staged = stageTransform(doc, tx, selection, transform, options);
        if (!staged) return staged;
        tx.commit(false);
    } catch (const std::invalid_argument& error) {
        return std::unexpected(invalid(error.what()));
    }
    return {};
}

DVec3 nudgeDelta(const SurfacePlane& plane, bool alongV, double gridSize, int direction) {
    if (!std::isfinite(gridSize) || gridSize <= 0.0 || (direction != 1 && direction != -1))
        throw std::invalid_argument("invalid nudge step or direction");
    return (alongV ? plane.vAxis : plane.uAxis) * (gridSize * direction);
}
DVec3 nudgeDelta(const WorkPlaneState& state, const SurfacePlane& fallback, bool alongV,
                 double gridSize, int direction) {
    const auto resolved = state.resolve();
    return nudgeDelta(resolved ? resolved->plane : fallback, alongV, gridSize, direction);
}

} // namespace apex::editor
