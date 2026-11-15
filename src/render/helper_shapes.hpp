#pragma once

#include "core/entity_class.hpp"
#include "document/document.hpp"
#include "document/entity_transform.hpp"
#include "geometry/rope.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apex::render {

struct HelperPeer { document::NodeId id; DVec3 position; };
struct HelperContext {
    const kv1::Node& block;
    const assets::EntityClass& cls;
    document::EntityPose pose;
    Vec4 defaultColour{1, 1, 1, 1};
    // Case-insensitive targetname lookup, excluding this entity; may be empty.
    std::function<std::vector<HelperPeer>(std::string_view name)> peersNamed;
};
enum class HelperVisibility { Always, WhenSelected, WhenSelfOrPeerSelected };
struct HelperSegment {
    DVec3 a, b;
    Vec4 colour;
    HelperVisibility visibility = HelperVisibility::Always;
    // The entity whose move carries b, and the one that carries a; empty means the owner.
    std::optional<document::NodeId> peer;
    std::optional<document::NodeId> anchor;
};
// A rope keyframe's rope to its NextKey (D14): `curve` is its resting shape from a to b. A move
// preview that carries the ends somewhere else settles it again from the moved ends.
struct HelperRope {
    DVec3 a, b;
    geometry::RopeSettings settings;
    std::vector<DVec3> curve;
    Vec4 colour;
    std::optional<document::NodeId> peer; // The entity whose move carries b; empty when it dangles.
};
struct HelperLabel { DVec3 position; std::string text; Vec4 colour; HelperVisibility visibility = HelperVisibility::Always; };
struct HelperShapes {
    std::vector<HelperSegment> segments;
    std::vector<HelperLabel> labels;
    std::vector<HelperRope> ropes;
    std::vector<int> sideIds;
    std::vector<std::string> unsupported;
};
HelperShapes buildHelperShapes(const HelperContext& context);

} // namespace apex::render
