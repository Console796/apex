#pragma once

#include "lighting/charting.hpp"
#include "lighting/cpu_accel.hpp"
#include "lighting/light.hpp"

#include <functional>
#include <string>

namespace apex::document { class Document; }

namespace apex::lighting {

struct BakeMaterial {
    bool sky = false, nodraw = false, noLight = false;
    Vec3 albedo{0.5f}; // Linear RGB reflectivity in [0,1]; unused by direct lighting.
};

struct BakeFace {
    std::vector<DVec3> vertices; // World-space convex loop, retained for later transport.
    DVec3 normal{0, 0, 1};
    LuxelChart chart;
    std::uint32_t material = 0;
    Vec3 albedo{0.5f};
    std::uint64_t solidId = 0;
    std::uint32_t sideId = 0; // Fragments share the source identity.
};

struct BakeCasterPrimitive {
    bool sky = false;
    std::optional<std::size_t> face;
};

struct BakeCasterGeometry {
    TriangleMesh mesh;
    std::vector<BakeCasterPrimitive> primitives;
};

struct BakeOcclusion {
    // A stable snapshot: no instance updates while the baker is stepping. Null means open space.
    std::shared_ptr<const CpuAccel> accel;
    // First-hit classification, including primitive for mixed-material instances. Called concurrently.
    std::function<bool(const RayHit&)> isSky;
    // Optional hit-to-BakeScene::faces mapping, including chart fragments. Concurrent read-only.
    // Unmapped hits (props/non-bake geometry) still occlude but contribute no bounce radiance.
    std::function<std::optional<std::size_t>(const RayHit&)> faceOf;
    std::shared_ptr<const BakeCasterGeometry> geometry;

    bool reachesSky(DVec3 point, DVec3 direction) const;
    bool visible(DVec3 point, DVec3 endpoint) const;
};

struct BakeScene {
    std::vector<BakeFace> faces;
    std::vector<Light> lights;
    std::vector<std::string> materials; // Face material IDs index this name table.
    BakeOcclusion occlusion;
};

// Explicit-face entry point; charting performs the same subdivision as chartFace.
std::vector<BakeFace> bakeFaces(const geometry::Face& face, std::span<const DVec3> vertices,
                                std::uint32_t material = 0, Vec3 albedo = Vec3{0.5f});

// Unexpanded document snapshot, using the same solidGeometry as the render mirror. Sky/nodraw name
// fallbacks always apply; the optional lookup supplies compiled VMT flags (including NOLIGHT) and
// albedo without an assets-layer dependency; the caller fills albedo from Texture::reflectivity
// (linear). Displacement sides are omitted. Invalid solids fail.
// The caller supplies the caster snapshot (world/func_detail, nodraw included, clip/trigger excluded)
// and its sky classifier. For bounce, also supply faceOf matching the returned faces (solidId/sideId
// identify source sides; subdivided sides need primitive-to-fragment mapping). This adapter does
// not build casters, expand instances or bake props.
// Optional FGD classes supply compile defaults to lightsFromEntity; entity targets are resolved.
Result<BakeScene> bakeSceneFromDocument(
    const document::Document& document, BakeOcclusion occlusion,
    const std::function<BakeMaterial(std::string_view)>& material = {},
    const assets::EntityClassTable* classes = nullptr);

} // namespace apex::lighting
