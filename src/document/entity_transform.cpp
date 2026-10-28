#include "document/entity_transform.hpp"

#include "core/strings.hpp"

#include <cmath>

namespace apex::document {

EntityPose entityPose(const kv1::Node& block) {
    EntityPose pose;
    if (const auto* value = block.find("origin")) {
        if (const auto v = value->asVec3()) pose.origin = {(*v)[0], (*v)[1], (*v)[2]};
    }
    if (const auto* value = block.find("angles")) {
        if (const auto v = value->asVec3()) {
            pose.anglesDeg = {(*v)[0], (*v)[1], (*v)[2]};
            // Source's local +Y is left: columns forward, left, up are a proper rotation (basisFromAngles'
            // forward, right, up would mirror anything transformed by it).
            const auto axes = angleVectors(pose.anglesDeg);
            pose.basis = {axes.forward, -axes.right, axes.up};
        }
    }
    const auto* classname = block.find("classname");
    const bool isStatic = classname && ciEquals(classname->decodedValue().value_or(""), "prop_static");
    const auto* scale = block.find(isStatic ? "uniformscale" : "modelscale");
    if (!scale) scale = block.find(isStatic ? "modelscale" : "uniformscale");
    if (scale) {
        if (const auto value = scale->asDouble(); value && std::isfinite(*value) && *value > 0.0)
            pose.scale = *value;
    }
    return pose;
}

DMat4 entityTransform(const EntityPose& pose) {
    DMat4 transform{pose.basis};
    for (int column = 0; column < 3; ++column) transform[column] *= pose.scale;
    transform[3] = DVec4{pose.origin, 1.0};
    return transform;
}

} // namespace apex::document
