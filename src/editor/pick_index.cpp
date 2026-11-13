#include "editor/pick_index.hpp"

#include "core/strings.hpp"
#include "document/displacement.hpp"
#include "document/entity_transform.hpp"
#include "editor/tool.hpp"
#include "geometry/tolerances.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace apex::editor {
namespace {

using document::Dirty;
using document::NodeHandle;
using geometry::ConvexPolyhedron;
using geometry::Face;
using geometry::kPlanarEps;

bool has(Dirty flags, Dirty bits) {
    return (static_cast<unsigned>(flags) & static_cast<unsigned>(bits)) != 0;
}

bool inFace(const ConvexPolyhedron& poly, const Face& face, DVec3 point) {
    for (std::size_t i = 0; i < face.vertices.size(); ++i) {
        const auto a = poly.vertices[face.vertices[i]];
        const auto edge = poly.vertices[face.vertices[(i + 1) % face.vertices.size()]] - a;
        if (glm::dot(glm::cross(edge, point - a), face.plane.normal) <
            -kPlanarEps * glm::length(edge)) return false;
    }
    return true;
}

bool clippedPolygon(std::vector<DVec3> polygon, std::span<const Plane> planes) {
    std::vector<DVec3> next;
    for (const auto& plane : planes) {
        if (polygon.empty()) return false;
        next.clear();
        auto a = polygon.back();
        double da = plane.signedDistance(a);
        for (const auto b : polygon) {
            const double db = plane.signedDistance(b);
            if ((da <= 0.0) != (db <= 0.0)) next.push_back(a + (b - a) * (da / (da - db)));
            if (db <= 0.0) next.push_back(b);
            a = b;
            da = db;
        }
        polygon.swap(next);
    }
    return !polygon.empty();
}

bool clippedFace(const ConvexPolyhedron& poly, const Face& face, std::span<const Plane> planes) {
    std::vector<DVec3> polygon;
    for (const auto index : face.vertices) polygon.push_back(poly.vertices[index]);
    return clippedPolygon(std::move(polygon), planes);
}

// Möller-Trumbore, both orientations; t follows the unnormalized ray.
std::optional<double> rayTriangle(const Ray& ray, DVec3 a, DVec3 b, DVec3 c) {
    const auto e1 = b - a, e2 = c - a;
    const auto p = glm::cross(ray.dir, e2);
    const double det = glm::dot(e1, p);
    if (det == 0.0) return std::nullopt;
    const auto s = ray.origin - a;
    const double u = glm::dot(s, p) / det;
    if (u < 0.0 || u > 1.0) return std::nullopt;
    const auto q = glm::cross(s, e1);
    const double v = glm::dot(ray.dir, q) / det;
    if (v < 0.0 || u + v > 1.0) return std::nullopt;
    return glm::dot(e2, q) / det;
}

bool rayHitsBox(const Ray& ray, const AABB& box, double tMax) {
    constexpr double pad = 1e-6;
    double near = 0.0, far = tMax;
    for (int axis = 0; axis < 3; ++axis) {
        const double lo = box.min[axis] - pad, hi = box.max[axis] + pad;
        if (ray.dir[axis] == 0.0) {
            if (ray.origin[axis] < lo || ray.origin[axis] > hi) return false;
            continue;
        }
        double a = (lo - ray.origin[axis]) / ray.dir[axis], b = (hi - ray.origin[axis]) / ray.dir[axis];
        if (a > b) std::swap(a, b);
        near = std::max(near, a);
        far = std::min(far, b);
        if (near > far) return false;
    }
    return true;
}

// Ericson, Real-Time Collision Detection 5.1.5.
DVec3 closestOnTriangle(DVec3 p, DVec3 a, DVec3 b, DVec3 c) {
    const auto ab = b - a, ac = c - a, ap = p - a;
    const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return a;
    const auto bp = p - b;
    const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) return a + ab * (d1 / (d1 - d3));
    const auto cp = p - c;
    const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denominator = va + vb + vc;
    if (denominator == 0.0) return a;
    return a + ab * (vb / denominator) + ac * (vc / denominator);
}

// D8: each displaced side meshed once per solid change, from the side's own dispinfo. A side that
// does not parse or mesh (non-quad base) keeps its flat face; map_problems reports it.
std::vector<DisplacementSurface> meshDisplacements(const kv1::Node& solid, const ConvexPolyhedron& poly) {
    std::vector<DisplacementSurface> result;
    for (const auto* side : solid.findAll("side")) {
        if (!document::isDisplaced(*side)) continue;
        const auto* id = side->find("id");
        const auto value = id ? std::optional<int>(id->asInt().value_or(0)) : std::nullopt;
        if (!value) continue;
        const auto face = std::ranges::find(poly.faces, static_cast<std::uint32_t>(*value), &Face::sideId);
        if (face == poly.faces.end()) continue;
        const auto disp = document::parseDisplacement(*side->find("dispinfo"));
        if (!disp) continue;
        const auto corners = geometry::displacementCorners(poly, *face);
        if (!corners) continue;
        auto mesh = geometry::displacementMesh(*corners, *disp);
        if (!mesh) continue;
        DisplacementSurface surface{static_cast<SideId>(*value), std::move(*mesh), {}};
        for (const auto& position : surface.mesh.positions) surface.bounds.expand(position);
        result.push_back(std::move(surface));
    }
    return result;
}

bool boxIntersects(const ConvexPolyhedron& box, std::span<const Plane> planes) {
    // If no box face survived, the frustum can still be wholly inside the box. Vertices
    // of the combined bounded halfspaces certify this case (also for oblique frusta).
    std::vector<Plane> combined(planes.begin(), planes.end());
    for (const auto& face : box.faces) combined.push_back(face.plane);
    for (std::size_t i = 0; i < combined.size(); ++i) {
        for (std::size_t j = i + 1; j < combined.size(); ++j) {
            for (std::size_t k = j + 1; k < combined.size(); ++k) {
                const auto& a = combined[i];
                const auto& b = combined[j];
                const auto& c = combined[k];
                const auto bc = glm::cross(b.normal, c.normal);
                const double det = glm::dot(a.normal, bc);
                if (det == 0.0) continue;
                const auto p = (a.dist * bc + b.dist * glm::cross(c.normal, a.normal) +
                                c.dist * glm::cross(a.normal, b.normal)) / det;
                if (std::ranges::all_of(combined, [&](const Plane& plane) {
                        return plane.signedDistance(p) <= kPlanarEps;
                    })) return true;
            }
        }
    }
    return false;
}

DVec3 closestOnFace(const ConvexPolyhedron& poly, const Face& face, DVec3 point) {
    const auto projected = point - face.plane.normal * face.plane.signedDistance(point);
    if (inFace(poly, face, projected)) return projected;
    DVec3 best{0.0};
    double squared = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < face.vertices.size(); ++i) {
        const auto a = poly.vertices[face.vertices[i]];
        const auto edge = poly.vertices[face.vertices[(i + 1) % face.vertices.size()]] - a;
        const double length2 = glm::dot(edge, edge);
        const auto candidate = a + edge * (length2 == 0.0 ? 0.0 :
            std::clamp(glm::dot(point - a, edge) / length2, 0.0, 1.0));
        const auto delta = point - candidate;
        const double d2 = glm::dot(delta, delta);
        if (d2 < squared) {
            best = candidate;
            squared = d2;
        }
    }
    return best;
}

bool hitLess(const Hit& a, const Hit& b) {
    return std::tie(a.t, a.node, a.side) < std::tie(b.t, b.node, b.side);
}

} // namespace

bool PickIndex::Item::displaced(std::uint32_t side) const {
    return std::ranges::any_of(displacements, [&](const DisplacementSurface& surface) {
        return surface.side == static_cast<SideId>(side);
    });
}

const DisplacementSurface* PickIndex::displacement(NodeId node, SideId side) const {
    const auto item = items_.find(node);
    if (item == items_.end()) return nullptr;
    for (const auto& surface : item->second.displacements)
        if (surface.side == side) return &surface;
    return nullptr;
}

std::optional<AABB> PickIndex::bounds(NodeId node, std::optional<SideId> side) const {
    const auto item = items_.find(node);
    if (item == items_.end()) return std::nullopt;
    const auto& poly = item->second.poly;
    if (!side) return item->second.box;
    if (const auto* surface = displacement(node, *side)) return surface->bounds;
    for (const auto& face : poly.faces) {
        if (static_cast<SideId>(face.sideId) != *side) continue;
        AABB result;
        for (const auto vertex : face.vertices) result.expand(poly.vertices[vertex]);
        return result;
    }
    return std::nullopt;
}

PickIndex::PickIndex(const document::Document& doc, EntityBoundsProvider bounds)
    : doc_(doc), bounds_(std::move(bounds)) {
    rebuild();
}

PickIndex::~PickIndex() { detach(); }

void PickIndex::attach(document::Document& doc) {
    if (&doc != &doc_) throw std::invalid_argument("PickIndex observer document differs");
    if (observed_) return;
    rebuild(); // Include edits made while detached.
    observer_ = doc.observe([this](const document::ChangeSet& changes) { apply(changes); });
    observed_ = &doc;
}

void PickIndex::detach() {
    if (observed_) observed_->unobserve(observer_);
    observed_ = nullptr;
}

void PickIndex::setViewState(ViewState view) {
    view_ = std::move(view);
    rebuild();
}

void PickIndex::setInstanceContents(std::span<const document::InstanceContent> contents,
                                    document::InstanceVisibility visibility) {
    std::map<std::shared_ptr<const document::Document>, std::shared_ptr<PickIndex>> indices;
    for (const auto& item : instances_) indices.emplace(item.content.keys, item.index);
    instances_.clear();
    instanceVisibility_ = visibility;
    for (const auto& content : contents) {
        if (!content.keys) continue;
        auto& index = indices[content.keys];
        if (!index) index = std::make_shared<PickIndex>(*content.keys, bounds_);
        instances_.push_back({content, index});
    }
}

void PickIndex::remove(NodeId id) {
    aimed_.erase(id);
    failed_.erase(id);
    if (const auto it = items_.find(id); it != items_.end()) {
        tree_.remove(it->second.leaf);
        leaves_.erase(it->second.leaf);
        items_.erase(it);
    }
}

void PickIndex::refresh(NodeId id, bool building) {
    const auto handle = doc_.handle(id);
    const auto* node = handle ? doc_.get(*handle) : nullptr;
    failed_.erase(id);
    if (!node || !node->block || !isVisible(doc_, id, view_)) {
        remove(id);
        return;
    }
    ConvexPolyhedron poly;
    EntityPickShape shape;
    document::EntityPose pose;
    std::vector<DisplacementSurface> displacements;
    const bool entity = std::holds_alternative<Handle<document::Entity>>(*handle);
    if (std::holds_alternative<Handle<document::Solid>>(*handle)) {
        auto geometry = geometry::solidGeometry(*node->block);
        if (!geometry) {
            remove(id);
            failed_.insert(id);
            return;
        }
        poly = std::move(geometry->poly);
        displacements = meshDisplacements(*node->block, poly);
    } else if (entity) {
        std::string classname;
        if (const auto* value = node->block->find("classname")) {
            classname = value->decodedValue().value_or("");
        }
        if (ciEquals(classname, "worldspawn") || ciEquals(node->block->key(), "world")) {
            remove(id);
            return;
        }
        bool brush = false;
        const auto walk = [&](const auto& self, NodeId childId) -> void {
            const auto childHandle = doc_.handle(childId);
            if (!childHandle) return;
            if (std::holds_alternative<Handle<document::Solid>>(*childHandle)) brush = true;
            if (const auto* child = doc_.get(*childHandle)) {
                for (const auto descendant : child->children) self(self, descendant);
            }
        };
        for (const auto child : node->children) walk(walk, child);
        if (brush) {
            remove(id);
            return;
        }
        pose = document::entityPose(*node->block);
        if (bounds_) shape = bounds_(*node->block).value_or(shape);
        if (shape.pose) pose = *shape.pose;
        if (shape.pose && node->block->find("target")) aimed_.insert(id);
        else aimed_.erase(id);
        LocalBox local = shape.fallback;
        if (shape.model) {
            auto bounds = shape.model->bounds(shape.body);
            if (bounds.empty()) {
                remove(id);
                return;
            }
            // A planar mesh still needs a volume for the box/frustum path.
            for (int axis = 0; axis < 3; ++axis) {
                if (bounds.max[axis] - bounds.min[axis] < 1e-4) {
                    bounds.min[axis] -= 1e-4;
                    bounds.max[axis] += 1e-4;
                }
            }
            local = {bounds.min, bounds.max};
        }
        auto boxPose = pose;
        if (!shape.model) boxPose.scale = 1.0;
        const auto transform = document::entityTransform(boxPose);
        std::array<DVec3, 8> corners;
        for (unsigned i = 0; i < corners.size(); ++i) {
            const DVec3 point{i & 1 ? local.maxs.x : local.mins.x,
                              i & 2 ? local.maxs.y : local.mins.y,
                              i & 4 ? local.maxs.z : local.mins.z};
            corners[i] = DVec3{transform * DVec4{point, 1.0}};
        }
        auto hull = geometry::convexHull(corners);
        if (!hull) {
            remove(id);
            failed_.insert(id);
            return;
        }
        poly = std::move(*hull);
    } else {
        remove(id);
        return;
    }
    auto bounds = poly.bounds();
    for (const auto& surface : displacements) bounds.expand(surface.bounds);
    if (const auto it = items_.find(id); it != items_.end()) {
        it->second.poly = std::move(poly);
        it->second.shape = std::move(shape);
        it->second.pose = pose;
        it->second.displacements = std::move(displacements);
        it->second.box = bounds;
        if (!building) tree_.update(it->second.leaf, bounds);
    } else {
        if (nextLeaf_ == std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("PickIndex leaf IDs exhausted; rebuild required");
        }
        const auto leaf = nextLeaf_++;
        items_.emplace(id, Item{leaf, std::move(poly), entity, std::move(shape), pose,
                                std::move(displacements), bounds});
        leaves_.emplace(leaf, id);
        if (!building) tree_.insert(leaf, bounds);
    }
}

void PickIndex::rebuild() {
    for (auto& instance : instances_) instance.index->rebuild();
    tree_.build({});
    items_.clear();
    leaves_.clear();
    handles_.clear();
    failed_.clear();
    aimed_.clear();
    nextLeaf_ = 0;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto handle = doc_.handle(id);
        const auto* node = handle ? doc_.get(*handle) : nullptr;
        if (!node) return;
        handles_.emplace(*handle, id);
        refresh(id, true);
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc_.roots()) walk(walk, root);
    std::vector<geometry::Bvh::Item> leaves;
    leaves.reserve(items_.size());
    for (const auto& [id, item] : items_) leaves.push_back({item.leaf, item.box});
    tree_.build(leaves);
}

void PickIndex::apply(const document::ChangeSet& changes) {
    if (changes.empty()) return;
    if (changes.created.size() + changes.modified.size() + changes.destroyed.size() > 1000) {
        rebuild();
        return;
    }
    // A target move/rename can rotate a light helper whose own keys did not change.
    std::set<NodeId> affected(aimed_.begin(), aimed_.end());
    std::unordered_set<NodeId> visited;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        if (!visited.insert(id).second) return;
        affected.insert(id);
        const auto h = doc_.handle(id);
        const auto* node = h ? doc_.get(*h) : nullptr;
        if (!node) return;
        handles_[*h] = id;
        for (const auto child : node->children) self(self, child);
    };
    const auto touch = [&](NodeHandle h, bool subtree) {
        const auto* node = doc_.get(h);
        if (!node) return;
        handles_[h] = node->id;
        if (subtree) walk(walk, node->id);
        else affected.insert(node->id);
        // Editor blocks of entities are managed children; edits there affect the owner.
        if (node->block && ciEquals(node->block->key(), "editor") && node->parent) {
            walk(walk, node->parent);
        }
    };
    for (const auto h : changes.created) touch(h, true);
    for (const auto& m : changes.modified) {
        if (has(m.flags, Dirty::Geometry | Dirty::KeyValues | Dirty::Visibility | Dirty::Hierarchy)) {
            touch(m.handle, has(m.flags, Dirty::Visibility | Dirty::Hierarchy));
        }
    }
    for (const auto h : changes.destroyed) {
        if (const auto it = handles_.find(h); it != handles_.end()) {
            affected.insert(it->second);
            handles_.erase(it);
        }
    }
    if (!changes.vanishedSides.empty()) {
        const std::unordered_set<SideId> vanished(changes.vanishedSides.begin(), changes.vanishedSides.end());
        for (const auto& [id, item] : items_) {
            for (const auto& face : item.poly.faces) {
                if (!item.entity && vanished.contains(static_cast<SideId>(face.sideId))) affected.insert(id);
            }
        }
    }
    const auto itemCount = std::ranges::count_if(affected, [&](NodeId id) {
        const auto h = doc_.handle(id);
        return items_.contains(id) || failed_.contains(id) || (h &&
            (std::holds_alternative<Handle<document::Solid>>(*h) ||
             std::holds_alternative<Handle<document::Entity>>(*h)));
    });
    if (affected.size() > 1000 || static_cast<std::size_t>(itemCount) > items_.size() / 4) {
        rebuild();
        return;
    }
    for (const auto id : affected) refresh(id);
    if (tree_.quality() > 2.0) tree_.rebuild();
}

std::optional<Hit> PickIndex::intersect(NodeId id, Ray ray, double tMax) const {
    const auto& item = items_.at(id);
    std::optional<Hit> best;
    if (item.shape.model) {
        const auto& model = *item.shape.model;
        const auto inverse = glm::inverse(document::entityTransform(item.pose));
        const Ray local{DVec3{inverse * DVec4{ray.origin, 1.0}},
                        DVec3{inverse * DVec4{ray.dir, 0.0}}};
        model.tree_.raycast(local, tMax, [&](std::uint32_t triangle, double limit) -> std::optional<double> {
            if (!model.selected(triangle, item.shape.body)) return std::nullopt;
            const auto offset = std::size_t(triangle) * 3;
            const auto a = model.positions_[model.indices_[offset]];
            const auto e1 = model.positions_[model.indices_[offset + 1]] - a;
            const auto e2 = model.positions_[model.indices_[offset + 2]] - a;
            const auto p = glm::cross(local.dir, e2);
            const double det = glm::dot(e1, p);
            if (det == 0.0) return std::nullopt;
            const auto s = local.origin - a;
            const double u = glm::dot(s, p) / det;
            const auto q = glm::cross(s, e1);
            const double v = glm::dot(local.dir, q) / det;
            if (u < 0.0 || v < 0.0 || u + v > 1.0) return std::nullopt;
            const double t = glm::dot(e2, q) / det;
            if (!(t >= 0.0 && t <= limit) || (best && t >= best->t)) return std::nullopt;
            const auto normal = glm::normalize(item.pose.basis * glm::cross(e1, e2));
            best = Hit{id, std::nullopt, ray.at(t), normal, t, true, false};
            return t;
        });
        return best;
    }
    for (const auto& face : item.poly.faces) {
        if (item.displaced(face.sideId)) continue;
        const double denominator = glm::dot(face.plane.normal, ray.dir);
        if (denominator == 0.0) continue;
        const double t = -face.plane.signedDistance(ray.origin) / denominator;
        if (!(t >= 0.0 && t <= tMax) || (best && t > best->t)) continue;
        const auto point = ray.at(t);
        if (!inFace(item.poly, face, point)) continue;
        Hit hit{id, item.entity ? std::nullopt : std::optional(static_cast<SideId>(face.sideId)),
                point, face.plane.normal, t};
        if (!best || hitLess(hit, *best)) best = hit;
    }
    for (const auto& surface : item.displacements) {
        if (!rayHitsBox(ray, surface.bounds, best ? std::min(best->t, tMax) : tMax)) continue;
        const auto& mesh = surface.mesh;
        for (const auto& triangle : mesh.triangles) {
            const auto a = mesh.positions[triangle[0]], b = mesh.positions[triangle[1]],
                       c = mesh.positions[triangle[2]];
            const auto t = rayTriangle(ray, a, b, c);
            if (!t || !(*t >= 0.0 && *t <= tMax) || (best && *t > best->t)) continue;
            const auto normal = glm::cross(b - a, c - a);
            const double length = glm::length(normal);
            if (!(length > 0.0)) continue;
            Hit hit{id, surface.side, ray.at(*t), normal / length, *t, false, true};
            if (!best || hitLess(hit, *best)) best = hit;
        }
    }
    return best;
}

std::vector<Hit> PickIndex::pick(Ray ray, double tMax) const {
    std::vector<Hit> hits;
    tree_.raycast(ray, tMax, [&](std::uint32_t leaf, double limit) -> std::optional<double> {
        if (auto hit = intersect(leaves_.at(leaf), ray, limit)) hits.push_back(*hit);
        return std::nullopt;
    });
    if (instanceVisibility_ != document::InstanceVisibility::Hidden) {
        for (const auto& item : instances_) {
            if (!isVisible(doc_, item.content.instance, view_)) continue;
            const auto inverse = glm::inverse(item.content.transform);
            const Ray local{DVec3(inverse * DVec4(ray.origin, 1.0)), DVec3(inverse * DVec4(ray.dir, 0.0))};
            if (auto hit = item.index->first(local); hit && hit->t <= tMax) {
                hit->node = item.content.instance;
                hit->side.reset();
                hit->point = ray.at(hit->t);
                hit->normal = glm::normalize(glm::transpose(DMat3(inverse)) * hit->normal);
                hits.push_back(*hit);
            }
        }
    }
    std::ranges::sort(hits, hitLess);
    std::set<NodeId> seen;
    std::erase_if(hits, [&](const Hit& hit) { return !seen.insert(hit.node).second; });
    return hits;
}

std::optional<Hit> PickIndex::first(Ray ray) const {
    if (!instances_.empty() && instanceVisibility_ != document::InstanceVisibility::Hidden) {
        auto hits = pick(ray);
        return hits.empty() ? std::nullopt : std::optional(hits.front());
    }
    std::optional<Hit> best;
    tree_.raycast(ray, std::numeric_limits<double>::infinity(),
        [&](std::uint32_t leaf, double limit) -> std::optional<double> {
            if (auto hit = intersect(leaves_.at(leaf), ray, limit)) {
                if (!best || hitLess(*hit, *best)) best = hit;
            }
            return best ? std::optional(best->t) : std::nullopt;
        });
    return best;
}

std::optional<Hit> PickIndex::first(Ray ray, const std::function<bool(NodeId)>& allow) const {
    std::optional<Hit> best;
    tree_.raycast(ray, std::numeric_limits<double>::infinity(),
        [&](std::uint32_t leaf, double limit) -> std::optional<double> {
            const auto id = leaves_.at(leaf);
            if (allow(id)) {
                if (auto hit = intersect(id, ray, limit)) {
                    if (!best || hitLess(*hit, *best)) best = hit;
                }
            }
            return best ? std::optional(best->t) : std::nullopt;
        });
    return best;
}

std::vector<NodeId> PickIndex::frustum(std::span<const Plane> planes, Mode mode) const {
    std::vector<NodeId> result;
    tree_.queryPlanes(planes, [&](std::uint32_t leaf, bool inside) {
        const auto id = leaves_.at(leaf);
        const auto& item = items_.at(id);
        const auto contained = [&](DVec3 p) {
            return std::ranges::all_of(planes, [&](const Plane& plane) { return plane.signedDistance(p) <= 0.0; });
        };
        if (!inside) {
            inside = std::ranges::all_of(item.poly.vertices, contained) &&
                std::ranges::all_of(item.displacements, [&](const DisplacementSurface& surface) {
                    return std::ranges::all_of(surface.mesh.positions, contained);
                });
        }
        if (!inside && mode == Mode::Touching) {
            inside = std::ranges::any_of(item.poly.faces, [&](const Face& face) {
                return !item.displaced(face.sideId) && clippedFace(item.poly, face, planes);
            }) || std::ranges::any_of(item.displacements, [&](const DisplacementSurface& surface) {
                const auto& mesh = surface.mesh;
                return std::ranges::any_of(mesh.triangles, [&](const auto& triangle) {
                    return clippedPolygon({mesh.positions[triangle[0]], mesh.positions[triangle[1]],
                                           mesh.positions[triangle[2]]}, planes);
                });
            });
            if (!inside && item.entity) inside = boxIntersects(item.poly, planes);
        }
        if (inside) result.push_back(id);
    });
    if (instanceVisibility_ != document::InstanceVisibility::Hidden) {
        for (const auto& item : instances_) {
            if (!isVisible(doc_, item.content.instance, view_)) continue;
            std::vector<Plane> local;
            for (const auto& plane : planes) {
                const auto normal = glm::transpose(DMat3(item.content.transform)) * plane.normal;
                const auto dist = plane.dist - glm::dot(plane.normal, DVec3(item.content.transform[3]));
                const auto length = glm::length(normal);
                local.push_back({normal / length, dist / length});
            }
            if (!item.index->frustum(local, mode).empty()) result.push_back(item.content.instance);
        }
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<NearHit> PickIndex::nearest(DVec3 point, double radius) const {
    std::vector<NearHit> result;
    tree_.queryRadius(point, radius, [&](std::uint32_t leaf, double) {
        const auto id = leaves_.at(leaf);
        const auto& item = items_.at(id);
        const auto& poly = item.poly;
        NearHit best{id, DVec3{0.0}, std::numeric_limits<double>::infinity()};
        for (const auto& face : poly.faces) {
            if (item.displaced(face.sideId)) continue;
            const auto candidate = closestOnFace(poly, face, point);
            const double distance = glm::length(candidate - point);
            if (distance < best.distance) best = {id, candidate, distance};
        }
        for (const auto& surface : item.displacements) {
            const auto& mesh = surface.mesh;
            for (const auto& triangle : mesh.triangles) {
                const auto candidate = closestOnTriangle(point, mesh.positions[triangle[0]],
                                                         mesh.positions[triangle[1]], mesh.positions[triangle[2]]);
                const double distance = glm::length(candidate - point);
                if (distance < best.distance) best = {id, candidate, distance};
            }
        }
        if (best.distance <= radius) result.push_back(best);
    });
    std::ranges::sort(result, [](const NearHit& a, const NearHit& b) {
        return std::tie(a.distance, a.node) < std::tie(b.distance, b.node);
    });
    return result;
}

FeatureHits PickIndex::nearestFeature(Ray ray, double pixelRadius, const ToolView& view,
                                      const std::function<bool(const FeatureRef&)>& allow,
                                      const std::function<bool(NodeId)>& occluders) const {
    FeatureHits result;
    if (!(pixelRadius >= 0.0 && std::isfinite(pixelRadius))) return result;
    const auto forward = glm::normalize(view.cameraForward);
    const auto right = glm::normalize(glm::cross(forward, view.cameraUp));
    const auto up = glm::cross(right, forward);
    const double along = glm::dot(ray.dir, forward);
    if (!(along > 0.0)) return result;
    const auto cursor = worldToPixel(view, ray.at(std::max(0.0,
        (1.0 - glm::dot(ray.origin - view.cameraPosition, forward)) / along)));
    if (!cursor) return result;
    const double scale = view.orthographic ? view.orthoHeight / view.size.y :
        2.0 * std::tan(glm::radians(view.fovYDegrees) * 0.5) / view.size.y;
    const double left = (cursor->x - pixelRadius - view.size.x * 0.5) * scale;
    const double rightLimit = (cursor->x + pixelRadius - view.size.x * 0.5) * scale;
    const double bottom = (view.size.y * 0.5 - cursor->y - pixelRadius) * scale;
    const double top = (view.size.y * 0.5 - cursor->y + pixelRadius) * scale;
    std::array<Plane, 5> planes;
    const auto boundary = [&](DVec3 axis, double limit) {
        const auto normal = view.orthographic ? axis : axis - forward * limit;
        return Plane{normal, glm::dot(normal, view.cameraPosition) +
            (view.orthographic ? limit : 0.0)};
    };
    planes[0] = boundary(-right, -left);
    planes[1] = boundary(right, rightLimit);
    planes[2] = boundary(-up, -bottom);
    planes[3] = boundary(up, top);
    planes[4] = {-forward, -glm::dot(forward, view.orthographic ? ray.origin : view.cameraPosition) - 1e-6};

    const auto consider = [&](DVec3 point, NodeId node, SideId side, FeatureRef feature,
                              std::optional<FeatureHit>& best) {
        if (allow && !allow(feature)) return;
        const auto pixel = worldToPixel(view, point);
        if (!pixel) return;
        const double distance = glm::length(*pixel - *cursor);
        if (distance > pixelRadius) return;
        const double depth = glm::dot(point - view.cameraPosition, forward);
        const double rayDepth = glm::dot(point - ray.origin, forward);
        if (view.orthographic && rayDepth < 0.0) return;
        const auto origin = view.orthographic ? point - forward * rayDepth : view.cameraPosition;
        const auto delta = point - origin;
        const double length = glm::length(delta);
        if (!(length > 0.0)) return;
        const Ray toFeature{origin, delta / length};
        const auto hit = occluders ? first(toFeature, occluders) : first(toFeature);
        if (hit && hit->t < length - 1e-5) return;
        const FeatureHit candidate{node, side, point, distance, depth, feature};
        if (!best || std::tie(candidate.pixels, candidate.depth, candidate.node, candidate.side) <
                     std::tie(best->pixels, best->depth, best->node, best->side)) best = candidate;
    };
    tree_.queryPlanes(planes, [&](std::uint32_t leaf, bool) {
        const auto node = leaves_.at(leaf);
        const auto& item = items_.at(node);
        if (item.entity) return;
        const auto& poly = item.poly;
        std::map<std::size_t, SideId> vertices;
        std::map<std::pair<std::size_t, std::size_t>, SideId> edges;
        for (const auto& face : poly.faces) {
            const auto side = static_cast<SideId>(face.sideId);
            for (std::size_t i = 0; i < face.vertices.size(); ++i) {
                const auto a = face.vertices[i], b = face.vertices[(i + 1) % face.vertices.size()];
                const auto add = [side](auto& map, auto key) {
                    const auto [it, inserted] = map.emplace(key, side);
                    if (!inserted) it->second = std::min(it->second, side);
                };
                add(vertices, a);
                add(edges, std::pair{std::min(a, b), std::max(a, b)});
            }
        }
        for (const auto& [index, side] : vertices)
            consider(poly.vertices[index], node, side, {node, FeatureKind::Vertex, index, 0}, result.vertex);
        for (const auto& [indices, side] : edges) {
            auto a = poly.vertices[indices.first], b = poly.vertices[indices.second];
            const FeatureRef feature{node, FeatureKind::Edge, indices.first, indices.second};
            consider((a + b) * 0.5, node, side, feature, result.midpoint);
            // Clip at the ray origin plane (camera plane in perspective).
            const auto clipOrigin = view.orthographic ? ray.origin : view.cameraPosition;
            double za = glm::dot(a - clipOrigin, forward);
            double zb = glm::dot(b - clipOrigin, forward);
            constexpr double near = 1e-5;
            if (za < near && zb < near) continue;
            if (za < near) { a = a + (b - a) * ((near - za) / (zb - za)); za = near; }
            if (zb < near) { b = b + (a - b) * ((near - zb) / (za - zb)); zb = near; }
            const auto pa = worldToPixel(view, a), pb = worldToPixel(view, b);
            if (!pa || !pb) continue;
            const auto segment = *pb - *pa;
            const double length2 = glm::dot(segment, segment);
            const double t = length2 == 0.0 ? 0.0 :
                std::clamp(glm::dot(*cursor - *pa, segment) / length2, 0.0, 1.0);
            const double worldT = view.orthographic ? t : t * za / ((1.0 - t) * zb + t * za);
            consider(a + (b - a) * worldT, node, side, feature, result.edge);
        }
        // D8: displacement grid vertices snap like brush vertices.
        for (const auto& surface : item.displacements) {
            for (std::size_t i = 0; i < surface.mesh.positions.size(); ++i)
                consider(surface.mesh.positions[i], node, surface.side,
                         {node, FeatureKind::DisplacementVertex, i, static_cast<std::size_t>(surface.side)},
                         result.vertex);
        }
    });
    return result;
}

} // namespace apex::editor
