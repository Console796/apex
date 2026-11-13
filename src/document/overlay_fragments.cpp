#include "document/overlay_fragments.hpp"

#include "core/strings.hpp"
#include "document/displacement.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace apex::document {
namespace {

std::string text(const kv1::Node& block, std::string_view key) {
    const auto* entry = block.find(key);
    return entry ? entry->decodedValue().value_or("") : "";
}

std::vector<SideId> sideList(const kv1::Node& entity, std::string_view key) {
    const auto parsed = geometry::parseSideList(text(entity, key));
    return {parsed.begin(), parsed.end()};
}

// Distance from p to a convex polygon in its plane (0 inside), plus the distance to the plane.
double polygonDistance(DVec3 p, std::span<const DVec3> polygon, DVec3 normal) {
    const DVec3 onPlane = p - normal * glm::dot(normal, p - polygon[0]);
    bool inside = true;
    double edgeDistance = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const DVec3 a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        if (glm::dot(glm::cross(b - a, onPlane - a), normal) < 0.0) inside = false;
        const DVec3 ab = b - a;
        const double t = std::clamp(glm::dot(onPlane - a, ab) / std::max(glm::dot(ab, ab), 1e-30), 0.0, 1.0);
        edgeDistance = std::min(edgeDistance, glm::length(onPlane - (a + ab * t)));
    }
    const double planar = inside ? 0.0 : edgeDistance;
    const double height = std::abs(glm::dot(normal, p - polygon[0]));
    return std::sqrt(planar * planar + height * height);
}

bool toolMaterial(std::string_view material) { return toLowerAscii(material).starts_with("tools/"); }

void collectEntities(const Document& doc, NodeId id, std::vector<NodeId>& out) {
    const auto h = doc.handle(id);
    if (!h) return;
    const auto* node = doc.get(*h);
    if (!node) return;
    if (std::holds_alternative<Handle<Entity>>(*h)) out.push_back(id);
    for (const auto child : node->children) collectEntities(doc, child, out);
}

void collectSolids(const Document& doc, NodeId id, std::vector<NodeId>& out) {
    const auto h = doc.handle(id);
    if (!h) return;
    const auto* node = doc.get(*h);
    if (!node) return;
    if (std::holds_alternative<Handle<Solid>>(*h)) out.push_back(id);
    for (const auto child : node->children) collectSolids(doc, child, out);
}

} // namespace

std::optional<OverlayKind> overlayKind(std::string_view classname) {
    if (ciEquals(classname, "info_overlay")) return OverlayKind::Overlay;
    if (ciEquals(classname, "info_overlay_transition")) return OverlayKind::Transition;
    if (ciEquals(classname, "infodecal")) return OverlayKind::Decal;
    return std::nullopt;
}
std::optional<OverlayKind> overlayKind(const kv1::Node& entity) { return overlayKind(text(entity, "classname")); }

const geometry::SolidGeometry* SurfaceLookup::solid(NodeId id, const kv1::Node& block) {
    auto [it, inserted] = solids_.try_emplace(id);
    if (inserted) {
        if (auto shape = geometry::solidGeometry(block); shape && shape->poly.isValid()) it->second = std::move(*shape);
    }
    return it->second ? &*it->second : nullptr;
}

OverlaySurface SurfaceLookup::make(NodeId solidId, const geometry::SolidGeometry& shape, const geometry::Face& face) {
    OverlaySurface surface;
    surface.side = static_cast<SideId>(face.sideId);
    surface.solid = solidId;
    surface.normal = face.plane.normal;
    surface.material = face.material;
    surface.uv = face.uv;
    for (const auto index : face.vertices) surface.polygon.push_back(shape.poly.vertices.at(index));
    if (const auto disp = displacement(doc_, surface.side); disp && *disp) {
        if (const auto corners = geometry::displacementCorners(shape.poly, face)) {
            if (auto mesh = geometry::displacementMesh(*corners, **disp)) surface.displacement = std::move(*mesh);
        }
    }
    return surface;
}

std::optional<OverlaySurface> SurfaceLookup::surface(SideId side) {
    const auto location = doc_.side(side);
    if (!location) return std::nullopt;
    const auto* node = doc_.get(location->solid);
    if (!node || !node->block) return std::nullopt;
    const auto* shape = solid(node->id, *node->block);
    if (!shape) return std::nullopt;
    for (const auto& face : shape->poly.faces)
        if (static_cast<SideId>(face.sideId) == side) return make(node->id, *shape, face);
    return std::nullopt; // A redundant side the polyhedron discarded.
}

std::vector<OverlaySurface> SurfaceLookup::all() {
    std::vector<NodeId> ids;
    for (const auto root : doc_.roots()) collectSolids(doc_, root, ids);
    std::vector<OverlaySurface> surfaces;
    for (const auto id : ids) {
        const auto h = doc_.handle(id);
        const auto* node = h ? doc_.get(*h) : nullptr;
        if (!node || !node->block) continue;
        const auto* shape = solid(id, *node->block);
        if (!shape) continue;
        for (const auto& face : shape->poly.faces) surfaces.push_back(make(id, *shape, face));
    }
    return surfaces;
}

std::optional<geometry::OverlayFragment> clipToSurface(const geometry::OverlayQuad& quad, const OverlaySurface& surface) {
    const auto side = static_cast<std::uint32_t>(surface.side);
    if (surface.displacement) return geometry::clipOverlay(quad, *surface.displacement, side);
    return geometry::clipOverlay(quad, surface.polygon, surface.normal, side);
}

namespace {
std::optional<DVec3> decalOrigin(const kv1::Node& entity) {
    const auto* originKey = entity.find("origin");
    if (!originKey) return std::nullopt;
    const auto parsed = originKey->asVec3();
    if (!parsed) return std::nullopt;
    return DVec3{(*parsed)[0], (*parsed)[1], (*parsed)[2]};
}

} // namespace

std::vector<OverlaySurface> decalCandidates(SurfaceLookup& surfaces, const kv1::Node& entity, double reach) {
    std::vector<OverlaySurface> result;
    const auto origin = decalOrigin(entity);
    if (!origin) return result;
    for (auto& surface : surfaces.all()) {
        if (surface.polygon.size() < 3 || toolMaterial(surface.material)) continue;
        // Within reach of the origin's plane hit, or coplanar with a face that is.
        if (std::abs(glm::dot(surface.normal, *origin - surface.polygon[0])) > reach) continue;
        DVec3 lo = surface.polygon[0], hi = lo;
        for (const auto& p : surface.polygon) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
        if (glm::any(glm::lessThan(*origin + reach, lo)) || glm::any(glm::greaterThan(*origin - reach, hi))) continue;
        result.push_back(std::move(surface));
    }
    return result;
}

OverlayGeometry decalGeometry(std::span<const OverlaySurface> all, const kv1::Node& entity, DVec2 size) {
    OverlayGeometry result;
    result.kind = OverlayKind::Decal;
    result.approximate = true;
    result.material = text(entity, "texture");
    const auto found = decalOrigin(entity);
    if (!found) {
        result.problem = "decal has no origin";
        return result;
    }
    const DVec3 origin = *found;
    const double reach = std::max({size.x, size.y, 1.0}) * 0.5;
    const OverlaySurface* nearest = nullptr;
    double best = reach;
    for (const auto& surface : all) {
        if (surface.polygon.size() < 3 || toolMaterial(surface.material)) continue;
        const double distance = polygonDistance(origin, surface.polygon, surface.normal);
        if (distance <= best) {
            best = distance;
            nearest = &surface;
        }
    }
    if (!nearest) {
        result.problem = "no brush surface within the decal's reach";
        return result;
    }
    const DVec3 n = nearest->normal;
    const DVec3 centre = origin - n * glm::dot(n, origin - nearest->polygon[0]);
    const auto quad = geometry::faceAlignedQuad(centre, n, nearest->uv.uaxis, size);
    result.quads.push_back(quad);
    const double dist = glm::dot(n, nearest->polygon[0]);
    for (const auto& surface : all) {
        if (toolMaterial(surface.material) || surface.polygon.size() < 3) continue;
        if (glm::dot(surface.normal, n) < 1.0 - 1e-6 || std::abs(glm::dot(surface.normal, surface.polygon[0]) - dist) > 0.5)
            continue;
        if (auto fragment = clipToSurface(quad, surface)) {
            result.listed.push_back(surface.side);
            result.pieces.push_back({std::move(*fragment), surface.solid});
        }
    }
    return result;
}

OverlayGeometry overlayGeometry(SurfaceLookup& surfaces, const kv1::Node& entity, const DecalSize& decalSize) {
    const auto kind = overlayKind(entity);
    if (!kind) {
        OverlayGeometry result;
        result.problem = "not an overlay";
        return result;
    }
    if (*kind == OverlayKind::Decal) {
        const auto material = text(entity, "texture");
        return decalGeometry(surfaces.all(), entity, decalSize ? decalSize(material) : DVec2{64.0});
    }
    OverlayGeometry result;
    result.kind = *kind;
    result.material = text(entity, "material");
    result.listed = sideList(entity, "sides");
    std::vector<SideId> water;
    if (*kind == OverlayKind::Transition) {
        water = sideList(entity, "sides2");
        result.listed.insert(result.listed.end(), water.begin(), water.end());
    }
    std::vector<std::pair<SideId, OverlaySurface>> resolved;
    for (const auto side : result.listed) {
        if (auto surface = surfaces.surface(side)) resolved.emplace_back(side, std::move(*surface));
        else result.missing.push_back(side);
    }
    if (*kind == OverlayKind::Overlay) {
        auto quad = geometry::parseOverlay(entity);
        if (!quad) {
            result.problem = quad.error().what();
            return result;
        }
        result.quads.push_back(*quad);
    } else {
        // The water plane is the first resolvable water face; land faces are `sides`.
        const auto waterFace = std::ranges::find_if(resolved, [&](const auto& entry) {
            return std::ranges::find(water, entry.first) != water.end();
        });
        if (waterFace == resolved.end()) {
            result.problem = "transition has no water face (sides2)";
            return result;
        }
        const Plane plane{waterFace->second.normal, glm::dot(waterFace->second.normal, waterFace->second.polygon.at(0))};
        std::vector<std::vector<DVec3>> land;
        for (const auto& [side, surface] : resolved)
            if (std::ranges::find(water, side) == water.end()) land.push_back(surface.polygon);
        result.quads = geometry::transitionQuads(geometry::parseTransition(entity), plane, land);
    }
    for (const auto& quad : result.quads) {
        for (const auto& [side, surface] : resolved) {
            if (auto fragment = clipToSurface(quad, surface)) result.pieces.push_back({std::move(*fragment), surface.solid});
        }
    }
    return result;
}

OverlayGeometry overlayGeometry(const Document& doc, const kv1::Node& entity, const DecalSize& decalSize) {
    SurfaceLookup surfaces(doc);
    return overlayGeometry(surfaces, entity, decalSize);
}

std::vector<NodeId> entityNodes(const Document& doc) {
    std::vector<NodeId> ids;
    for (const auto root : doc.roots()) collectEntities(doc, root, ids);
    return ids;
}

std::vector<NodeId> overlaysOnSide(const Document& doc, SideId side) {
    std::vector<NodeId> result;
    for (const auto id : entityNodes(doc)) {
        const auto* node = doc.get(*doc.handle(id));
        if (!node || !node->block) continue;
        const auto kind = overlayKind(*node->block);
        if (!kind || *kind == OverlayKind::Decal) continue;
        auto listed = sideList(*node->block, "sides");
        if (*kind == OverlayKind::Transition) {
            const auto water = sideList(*node->block, "sides2");
            listed.insert(listed.end(), water.begin(), water.end());
        }
        if (std::ranges::find(listed, side) != listed.end()) result.push_back(id);
    }
    return result;
}

std::vector<NodeId> decalsOnSide(const Document& doc, SideId side, const DecalSize& decalSize) {
    std::vector<NodeId> result;
    std::optional<std::vector<OverlaySurface>> surfaces; // Built only when the map has a decal.
    for (const auto id : entityNodes(doc)) {
        const auto* node = doc.get(*doc.handle(id));
        if (!node || !node->block || overlayKind(*node->block) != OverlayKind::Decal) continue;
        if (!surfaces) surfaces = SurfaceLookup(doc).all();
        const DVec2 size = decalSize ? decalSize(text(*node->block, "texture")) : DVec2{64.0};
        const auto geometry = decalGeometry(*surfaces, *node->block, size);
        if (std::ranges::any_of(geometry.pieces, [&](const OverlayPiece& piece) { return piece.fragment.side == side; }))
            result.push_back(id);
    }
    return result;
}

} // namespace apex::document
