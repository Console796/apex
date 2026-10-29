#pragma once

// The light model shared by the preview and the baker (0.6 D1). Every key-to-parameter rule here is
// measured from native VRAD's worldlight lump (0.6 D2, docs/plans/probe-results/lighting_0_6_p0.md),
// and a Light holds exactly what that lump stores, so the two compare field by field.

#include "core/entity_class.hpp"
#include "core/kv1.hpp"
#include "core/math.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apex::lighting {

// Values match the worldlight record's emit type.
enum class LightKind : std::uint8_t { Point = 1, Spot = 2, Sun = 3, Ambient = 5 };

struct Light {
    LightKind kind = LightKind::Point;
    DVec3 origin{0.0};
    // Linear HDR intensity: the 2.2-decoded colour times brightness/255, times _lightscaleHDR, times the
    // 100-unit normalisation for point and spot lights. Irradiance is intensity / attenuation(d).
    DVec3 intensity{0.0};
    DVec3 direction{0.0}; // spot and sun aim; zero otherwise
    DVec3 shadowCastOffset{0.0};
    double stopdot = 0;  // cos(inner cone)
    double stopdot2 = 0; // cos(outer cone)
    double exponent = 0;
    double radius = 0; // _distance; 0 means unlimited
    double constant = 0, linear = 0, quadratic = 0;
    int style = 0;
    std::string styleName; // a named light gets a map-wide style from VRAD; the name is what shares one
    bool castEntityShadow = false;
    std::uint8_t specularMode = 0, directMode = 1, indirectMode = 1; // light_rt modes (0.6 D8)

    double attenuation(double distance) const { return constant + linear * distance + quadratic * distance * distance; }
};

// The worldlights VRAD writes for one entity, from its keys as they will reach the compiler. VRAD reads
// no FGD, so the class overload adds the defaults the compile snapshot writes
// (document::missingEntityDefaults), not the wider helper defaults.
// light_environment yields a sun and an ambient record; unknown classes and removed lights yield none.
// `target` is the resolved position of the spotlight's target entity, if it has one.
std::vector<Light> lightsFromEntity(const kv1::Node& block, std::optional<DVec3> target = {});
std::vector<Light> lightsFromEntity(const kv1::Node& block, const assets::EntityClass& cls,
                                    std::optional<DVec3> target = {});

struct Attenuation {
    double constant = 1, linear = 0, quadratic = 0;
};
// VRAD's solve for _fifty_percent_distance / _zero_percent_distance (measured; see the probe document).
Attenuation fiftyZeroAttenuation(double fifty, double zero);

// VRAD's response to light arriving at `cosine` to the normal, measured against its -bounce 0 lightmaps
// (lighting_0_6_2_reference.md): (c + c²)/2, not Lambert's c. Zero at or below the horizon.
double surfaceResponse(double cosine);

// Sky ambient on a face whose normal has this z, when the whole upper hemisphere is sky: VRAD weights
// each sky direction by surfaceResponse, normalised so a floor under open sky gets the full intensity
// (1 up, 0.5 vertical, 0 down). Partly covered skies scale this by visibility (0.6 D16).
double skyAmbientResponse(double normalZ);

// Direct irradiance at a point with the given unit normal, before visibility, in the units of VRAD's
// decoded HDR lightmap. Point and spot: intensity · surfaceResponse(cos) / attenuation(d); a spot also
// multiplies by the cosine to its axis, and between its cones by the linear edge ramp raised to the
// exponent (0 and 1 both mean linear). Sun: intensity · surfaceResponse. Measured by tools/light_parity.py.
DVec3 irradiance(const Light& light, DVec3 point, DVec3 normal);

// Distance beyond which the light's brightest channel falls below `threshold` (and within `radius`).
// Infinite for the sun, ambient, and lights whose attenuation never grows.
double effectiveRange(const Light& light, double threshold);

} // namespace apex::lighting
