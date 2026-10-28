#pragma once

#include "core/kv1.hpp"
#include "core/math.hpp"

namespace apex::document {

struct EntityPose {
    DVec3 origin{0.0};
    DVec3 anglesDeg{0.0};
    DMat3 basis{1.0};
    double scale = 1.0;
};

// Missing or malformed angles leave the basis at identity; valid angles give the rotation with columns forward, left, up.
EntityPose entityPose(const kv1::Node& block);
DMat4 entityTransform(const EntityPose& pose);

} // namespace apex::document
