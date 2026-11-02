#include "lighting/bake_diff.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>

namespace apex::lighting {
namespace {

struct Sphere {
    DVec3 centre{0};
    double radius = 0;
};

Sphere boundsOf(const BakeFace& face) {
    if (face.vertices.empty()) return {};
    DVec3 low = face.vertices.front(), high = low;
    for (const auto& p : face.vertices) { low = glm::min(low, p); high = glm::max(high, p); }
    Sphere sphere{(low + high) * 0.5, 0};
    for (const auto& p : face.vertices) sphere.radius = std::max(sphere.radius, glm::length(p - sphere.centre));
    return sphere;
}

struct Hasher {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    void add(std::uint64_t bits) { value = (value ^ bits) * 0x100000001b3ULL; value ^= value >> 29; }
    void add(double v) { add(std::bit_cast<std::uint64_t>(v)); }
    void add(const DVec3& v) { add(v.x); add(v.y); add(v.z); }
    void add(const DVec2& v) { add(v.x); add(v.y); }
};

// Geometry, normal and chart: everything that moves a luxel or changes its shadowing.
std::uint64_t geometryHash(const BakeFace& face) {
    Hasher h;
    h.add(face.vertices.size());
    for (const auto& p : face.vertices) h.add(p);
    h.add(face.normal);
    const auto& c = face.chart;
    for (const auto v : c.mins) h.add(std::uint64_t(std::int64_t(v)));
    for (const auto v : c.extents) h.add(std::uint64_t(std::int64_t(v)));
    for (const auto v : c.dimensions) h.add(std::uint64_t(std::int64_t(v)));
    for (const auto& v : c.worldToLuxel) h.add(v);
    h.add(c.offset);
    h.add(c.origin);
    for (const auto& v : c.luxelToWorld) h.add(v);
    return h.value;
}

const std::string& materialName(const BakeSceneView& scene, const BakeFace& face) {
    static const std::string none;
    return face.material < scene.materials.size() ? scene.materials[face.material] : none;
}

bool isSkyName(const std::string& name) { return name.starts_with("tools/toolsskybox"); }

bool sameLight(const Light& a, const Light& b) {
    return a.kind == b.kind && a.id == b.id && a.origin == b.origin && a.intensity == b.intensity &&
           a.direction == b.direction && a.shadowCastOffset == b.shadowCastOffset && a.stopdot == b.stopdot &&
           a.stopdot2 == b.stopdot2 && a.exponent == b.exponent && a.radius == b.radius &&
           a.constant == b.constant && a.linear == b.linear && a.quadratic == b.quadratic &&
           a.style == b.style && a.styleName == b.styleName && a.castEntityShadow == b.castEntityShadow &&
           a.specularMode == b.specularMode && a.directMode == b.directMode && a.indirectMode == b.indirectMode;
}

constexpr double kSlack = 2; // The baker samples one unit off the surface.

// Angular test: may any point of the sphere lie within `halfAngle` of `axis` seen from `origin`?
bool sphereInCone(DVec3 origin, DVec3 axis, double halfAngle, const Sphere& s) {
    const DVec3 v = s.centre - origin;
    const double distance = glm::length(v), reach = s.radius + kSlack;
    if (distance <= reach) return true;
    const double angle = std::acos(std::clamp(glm::dot(v / distance, axis), -1.0, 1.0));
    return angle - std::asin(std::min(1.0, reach / distance)) < halfAngle;
}

// Can `light` add direct light to this face? Conservative: bounding sphere, range, facing, cone.
bool mayReceive(const Light& light, const BakeFace& face, const Sphere& s) {
    if (!hasDirect(light)) return false;
    switch (light.kind) {
    case LightKind::Ambient: return true;
    case LightKind::Sun: return glm::dot(face.normal, -light.direction) > 0;
    case LightKind::Point:
    case LightKind::Spot: break;
    }
    const double range = effectiveRange(light, kLightThreshold);
    if (glm::length(s.centre - light.origin) > range + s.radius + kSlack) return false;
    // Entirely behind the face plane: cosine to the normal is non-positive for every vertex.
    bool front = face.vertices.empty();
    for (const auto& p : face.vertices) front = front || glm::dot(face.normal, light.origin - p) > -kSlack;
    if (!front) return false;
    if (light.kind == LightKind::Spot && light.stopdot2 > -1) {
        const double outer = std::acos(std::clamp(light.stopdot2, -1.0, 1.0));
        if (!sphereInCone(light.origin, light.direction, outer, s)) return false;
    }
    return true;
}

// Does the occluder possibly lie between this light and any point of the face?
bool shadowed(const Light& light, const Sphere& face, const Sphere& occluder) {
    if (light.kind == LightKind::Sun) {
        const DVec3 toSun = -light.direction;
        const DVec3 v = occluder.centre - face.centre;
        const double along = glm::dot(v, toSun), reach = face.radius + occluder.radius + kSlack;
        if (along < -reach) return false;
        return glm::length(v - toSun * along) <= reach;
    }
    // Point/spot: the cone from the light through the occluder, beyond the occluder's near side.
    const DVec3 d = occluder.centre - light.origin;
    const double distance = glm::length(d);
    if (distance <= occluder.radius + kSlack) return true;
    const double nearest = distance - occluder.radius;
    if (glm::length(face.centre - light.origin) + face.radius + kSlack < nearest) return false;
    return sphereInCone(light.origin, d / distance, std::asin(std::min(1.0, occluder.radius / distance)), face);
}

double gap(const Sphere& a, const Sphere& b) { return glm::length(a.centre - b.centre) - a.radius - b.radius; }

} // namespace

std::vector<FaceKey> faceKeys(std::span<const BakeFace> faces) {
    std::vector<FaceKey> keys;
    keys.reserve(faces.size());
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::uint32_t> ordinal;
    for (const auto& face : faces)
        keys.push_back({0, face.solidId, face.sideId, ordinal[{face.solidId, face.sideId}]++});
    return keys;
}

BakeDiff diffScenes(const BakeSceneView& before, const BakeSceneView& after) {
    BakeDiff diff;
    const auto n = after.faces.size();
    diff.oldOfNew.assign(n, -1);
    diff.dirty.assign(n, 0);
    diff.bounceDirty.assign(n, 0);
    const auto full = [&](const char* reason) {
        diff.full = true;
        diff.reason = reason;
        std::ranges::fill(diff.dirty, 1);
        std::ranges::fill(diff.bounceDirty, 1);
        diff.dirtyCount = diff.bounceDirtyCount = n;
        return diff;
    };
    if (before.keys.size() != before.faces.size() || after.keys.size() != n) return full("missing face keys");
    if (before.faces.empty() || n == 0) return full("no previous faces");

    std::map<FaceKey, int> oldIndex;
    for (std::size_t i = 0; i < before.keys.size(); ++i) oldIndex.emplace(before.keys[i], int(i));
    std::size_t matched = 0;
    std::vector<char> oldUsed(before.faces.size(), 0);
    for (std::size_t i = 0; i < n; ++i)
        if (const auto it = oldIndex.find(after.keys[i]); it != oldIndex.end()) {
            diff.oldOfNew[i] = it->second;
            oldUsed[std::size_t(it->second)] = 1;
            ++matched;
        }
    if (matched * 2 < n) return full("face set changed");

    std::vector<Sphere> spheres(n);
    for (std::size_t i = 0; i < n; ++i) spheres[i] = boundsOf(after.faces[i]);

    // Own changes, and the occluder spheres they leave behind.
    std::vector<Sphere> occluders;
    for (std::size_t i = 0; i < n; ++i) {
        const auto old = diff.oldOfNew[i];
        if (old < 0) { diff.dirty[i] = 1; occluders.push_back(spheres[i]); continue; }
        const auto& a = before.faces[std::size_t(old)];
        const auto& b = after.faces[i];
        const auto& nameA = materialName(before, a);
        const auto& nameB = materialName(after, b);
        if (nameA != nameB && (isSkyName(nameA) || isSkyName(nameB))) return full("sky material changed");
        if (geometryHash(a) != geometryHash(b)) {
            diff.dirty[i] = 1;
            occluders.push_back(boundsOf(a));
            occluders.push_back(spheres[i]);
        } else if (nameA != nameB || a.albedo != b.albedo) diff.dirty[i] = 1; // Bounce source only.
    }
    for (std::size_t i = 0; i < before.faces.size(); ++i)
        if (!oldUsed[i]) occluders.push_back(boundsOf(before.faces[i]));

    // Lights, matched by id (id 0 only matches an identical light).
    std::vector<const Light*> changedLights;
    const auto consider = [&](const Light& light) { if (hasDirect(light)) changedLights.push_back(&light); };
    std::vector<char> newMatched(after.lights.size(), 0);
    for (const auto& old : before.lights) {
        const Light* partner = nullptr;
        for (std::size_t i = 0; i < after.lights.size() && !partner; ++i)
            if (!newMatched[i] && sameLight(old, after.lights[i])) { newMatched[i] = 1; partner = &after.lights[i]; }
        if (!partner) consider(old);
    }
    for (std::size_t i = 0; i < after.lights.size(); ++i)
        if (!newMatched[i]) consider(after.lights[i]);
    for (const auto* light : changedLights)
        if (light->kind == LightKind::Ambient) return full("sky/ambient light changed");

    for (std::size_t i = 0; i < n; ++i) {
        if (diff.dirty[i]) continue;
        const auto& face = after.faces[i];
        for (const auto* light : changedLights)
            if (mayReceive(*light, face, spheres[i])) { diff.dirty[i] = 1; break; }
        if (diff.dirty[i]) continue;
        for (const auto& occluder : occluders) {
            const double reach = std::max(kGeometryMinRadius, kGeometryScale * occluder.radius);
            if (gap(spheres[i], occluder) <= reach) { diff.dirty[i] = 1; break; }
            for (const auto& light : after.lights)
                if (light.kind != LightKind::Ambient && mayReceive(light, face, spheres[i]) &&
                    shadowed(light, spheres[i], occluder)) { diff.dirty[i] = 1; break; }
            if (diff.dirty[i]) break;
        }
    }

    // Bounce coupling.
    std::vector<Sphere> sources;
    for (std::size_t i = 0; i < n; ++i) if (diff.dirty[i]) { ++diff.dirtyCount; sources.push_back(spheres[i]); }
    for (std::size_t i = 0; i < n; ++i) {
        if (diff.dirty[i]) { diff.bounceDirty[i] = 1; ++diff.bounceDirtyCount; continue; }
        for (const auto& source : sources)
            if (gap(spheres[i], source) <= kBounceRadius) { diff.bounceDirty[i] = 1; ++diff.bounceDirtyCount; break; }
    }
    return diff;
}

} // namespace apex::lighting
