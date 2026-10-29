#pragma once

#include "editor/visibility.hpp"
#include "editor/pick_model.hpp"
#include "document/entity_transform.hpp"
#include "document/instance_contents.hpp"
#include "geometry/bvh.hpp"

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <set>
#include <string_view>
#include <unordered_map>

namespace apex::editor {

using document::SideId;
struct ToolView;

struct LocalBox {
    DVec3 mins{-4.0}, maxs{4.0};
};
struct EntityPickShape {
    std::shared_ptr<const PickModel> model;
    std::uint32_t body = 0;
    LocalBox fallback;
    // Editor model helpers may use light pitch and target direction instead of entity angles.
    std::optional<document::EntityPose> pose = {};
};
using EntityBoundsProvider = std::function<std::optional<EntityPickShape>(const kv1::Node& entity)>;

struct Hit {
    NodeId node;
    std::optional<SideId> side;
    DVec3 point, normal;
    double t;
    bool modelTriangle = false; // Model surfaces have no brush SideId.
};
struct NearHit {
    NodeId node;
    DVec3 point;
    double distance;
};
enum class FeatureKind { Vertex, Edge };
struct FeatureRef {
    NodeId node = 0;
    FeatureKind kind = FeatureKind::Vertex;
    std::size_t a = 0, b = 0; // Edge endpoints are sorted; vertex uses a.
    auto operator<=>(const FeatureRef&) const = default;
};
struct FeatureHit {
    NodeId node;
    SideId side;
    DVec3 point;
    double pixels, depth;
    FeatureRef feature;
};
struct FeatureHits {
    std::optional<FeatureHit> vertex, midpoint, edge;
};
enum class Mode { Enclosed, Touching };

// The document must outlive the index (including an attached observer). Queries use owned
// polyhedra; no pointers into the document survive a call. Call apply after commits/undo/redo,
// or attach to the same mutable document for automatic updates. Construction builds the index.
class PickIndex {
public:
    using Mode = editor::Mode;
    explicit PickIndex(const document::Document& document, EntityBoundsProvider bounds = {});
    ~PickIndex();
    PickIndex(const PickIndex&) = delete;
    PickIndex& operator=(const PickIndex&) = delete;
    PickIndex(PickIndex&&) = delete;
    PickIndex& operator=(PickIndex&&) = delete;

    void rebuild();
    void apply(const document::ChangeSet& changes);
    void attach(document::Document& document);
    void detach();
    const ViewState& viewState() const { return view_; }
    void setViewState(ViewState view);
    void setInstanceContents(std::span<const document::InstanceContent> contents,
                             document::InstanceVisibility visibility = document::InstanceVisibility::Normal);
    std::size_t size() const { return items_.size(); }
    std::size_t skippedSolids() const { return failed_.size(); }

    // One nearest forward face per solid, counting both orientations. From inside, this
    // is the exit face. Edge/corner ties choose SideId. t follows the unnormalized Ray.
    std::vector<Hit> pick(Ray ray, double tMax = std::numeric_limits<double>::infinity()) const;
    std::optional<Hit> first(Ray ray) const;
    std::optional<AABB> bounds(NodeId node, std::optional<SideId> side = std::nullopt) const;
    // Inside means signedDistance <= 0. Touching clips face polygons; a frustum entirely
    // inside a solid is intentionally not reported. Entity boxes use volume intersection.
    std::vector<NodeId> frustum(std::span<const Plane> outwardPlanes, Mode mode) const;
    // Distance to the surface, including for interior points; point lies on a face.
    std::vector<NearHit> nearest(DVec3 point, double radius) const;

    // Solid features only. Occlusion is tested along each feature's camera ray: occluded
    // geometry is never a fallback. Equal pixel distances prefer depth, then node/side.
    FeatureHits nearestFeature(Ray ray, double pixelRadius, const ToolView& view,
                               const std::function<bool(const FeatureRef&)>& allow = {}) const;

private:
    struct Item {
        std::uint32_t leaf;
        geometry::ConvexPolyhedron poly;
        bool entity;
        EntityPickShape shape;
        document::EntityPose pose;
    };
    const document::Document& doc_;
    EntityBoundsProvider bounds_;
    ViewState view_;
    geometry::Bvh tree_;
    std::unordered_map<NodeId, Item> items_;
    std::unordered_map<std::uint32_t, NodeId> leaves_;
    std::map<document::NodeHandle, NodeId> handles_;
    std::unordered_set<NodeId> failed_;
    std::set<NodeId> aimed_;
    std::uint32_t nextLeaf_ = 0;
    document::Document* observed_ = nullptr;
    std::size_t observer_ = 0;
    struct InstancePick {
        document::InstanceContent content;
        std::shared_ptr<PickIndex> index;
    };
    std::vector<InstancePick> instances_;
    document::InstanceVisibility instanceVisibility_ = document::InstanceVisibility::Normal;

    void refresh(NodeId id, bool building = false);
    void remove(NodeId id);
    std::optional<Hit> intersect(NodeId id, Ray ray, double tMax) const;
};

} // namespace apex::editor
