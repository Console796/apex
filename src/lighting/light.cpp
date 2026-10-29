#include "lighting/light.hpp"

#include "core/strings.hpp"
#include "document/entity_helpers.hpp"
#include "document/entity_transform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace apex::lighting {
namespace {

std::string text(const kv1::Node& block, std::string_view key) {
    const auto* node = block.find(key);
    return node ? node->decodedValue().value_or("") : std::string{};
}

double number(const kv1::Node& block, std::string_view key, double fallback = 0) {
    const auto* node = block.find(key);
    if (!node) return fallback;
    const auto value = node->asDouble();
    return value && std::isfinite(*value) ? *value : fallback;
}

std::vector<double> numbers(std::string_view value) {
    std::vector<double> out;
    std::istringstream stream{std::string(value)};
    for (double v; stream >> v;) out.push_back(v);
    return out;
}

// "R G B A": 2.2-decoded colour times A/255. Three values mean A=255; one value is grey at A=255.
std::optional<DVec3> colour(std::string_view value) {
    const auto v = numbers(value);
    if (v.empty()) return std::nullopt;
    const DVec3 rgb = v.size() >= 3 ? DVec3{v[0], v[1], v[2]} : DVec3{v[0]};
    const double brightness = v.size() >= 4 ? v[3] : 255.0;
    const auto decode = [](double c) { return std::pow(std::max(c, 0.0) / 255.0, 2.2); };
    return DVec3{decode(rgb.x), decode(rgb.y), decode(rgb.z)} * (brightness / 255.0);
}

// The HDR lump (the only one VRAD writes) uses the HDR key unless it holds the "-1 -1 -1 1" sentinel,
// then applies the HDR scale.
DVec3 hdrColour(const kv1::Node& block, std::string_view ldrKey, std::string_view hdrKey, std::string_view scaleKey) {
    auto result = colour(text(block, ldrKey));
    const auto hdr = text(block, hdrKey);
    if (const auto v = numbers(hdr); v.size() >= 3 && !(v[0] == -1 && v[1] == -1 && v[2] == -1))
        result = colour(hdr);
    return result.value_or(DVec3{0.0}) * number(block, scaleKey, 1.0);
}

DVec3 aim(const kv1::Node& block, std::optional<DVec3> target) {
    auto pose = document::entityPose(block);
    return document::spotlightPose(block, pose, target).basis[0];
}

void setAttenuation(Light& light, const kv1::Node& block) {
    if (const double fifty = number(block, "_fifty_percent_distance"); fifty > 0) {
        const auto a = fiftyZeroAttenuation(fifty, number(block, "_zero_percent_distance"));
        light.constant = a.constant;
        light.linear = a.linear;
        light.quadratic = a.quadratic;
        return; // the solve already puts 50% at `fifty`, so no 100-unit normalisation
    }
    light.constant = number(block, "_constant_attn");
    light.linear = number(block, "_linear_attn");
    light.quadratic = number(block, "_quadratic_attn");
    if (light.constant < 0 || (light.constant == 0 && light.linear == 0 && light.quadratic == 0)) light.constant = 1;
    // Brightness is defined at 100 units.
    light.intensity *= light.constant + 100 * light.linear + 10000 * light.quadratic;
}

Light common(const kv1::Node& block, LightKind kind) {
    Light light;
    light.kind = kind;
    light.origin = document::entityPose(block).origin;
    light.style = static_cast<int>(number(block, "style"));
    light.styleName = text(block, "targetname");
    light.specularMode = static_cast<std::uint8_t>(number(block, "_specularmode", 0));
    light.directMode = static_cast<std::uint8_t>(number(block, "_directmode", 1));
    light.indirectMode = static_cast<std::uint8_t>(number(block, "_indirectmode", 1));
    if (const auto offset = numbers(text(block, "_shadoworiginoffset")); offset.size() >= 3)
        light.shadowCastOffset = {offset[0], offset[1], offset[2]};
    return light;
}

} // namespace

Attenuation fiftyZeroAttenuation(double fifty, double zero) {
    // Measured: c + l·f + q·f² = 2 and (c + l·z + q·z²)/c = 256, with c from the first of these steps
    // whose curve is no brighter one unit out than at the light (lighting_0_6_p0.md).
    if (zero == fifty) return {0, 2 / fifty, 0}; // measured: purely linear, the r = 1 limit
    const double r = zero > fifty ? zero / fifty : 2.0;
    Attenuation a;
    for (int n = 0; n <= 19; ++n) {
        const double c = 1.0 / (1.0 + n * (255.0 - r) / (40.0 * r));
        const double d = (255.0 * c - r * (2.0 - c)) / (r * r - r); // q·f²
        const double b = 2.0 - c - d;                              // l·f
        a = {c, b / fifty, d / (fifty * fifty)};
        if (a.linear + a.quadratic >= 0) break;
    }
    return a;
}

std::vector<Light> lightsFromEntity(const kv1::Node& block, std::optional<DVec3> target) {
    const auto classname = text(block, "classname");
    if (number(block, "_removeaftercompile") != 0) return {};
    const bool point = ciEquals(classname, "light") || ciEquals(classname, "light_rt");
    const bool spot = ciEquals(classname, "light_spot") || ciEquals(classname, "light_rt_spot");
    if (point || spot) {
        auto light = common(block, spot ? LightKind::Spot : LightKind::Point);
        light.intensity = hdrColour(block, "_light", "_lightHDR", "_lightscaleHDR");
        light.radius = std::max(number(block, "_distance"), 0.0);
        light.castEntityShadow = number(block, "_castentityshadow", 1) != 0;
        setAttenuation(light, block);
        if (spot) {
            double inner = number(block, "_inner_cone");
            if (inner == 0) inner = 10;
            double outer = number(block, "_cone");
            if (outer == 0 || outer < inner) outer = inner;
            light.stopdot = std::cos(glm::radians(inner));
            light.stopdot2 = std::cos(glm::radians(outer));
            light.exponent = number(block, "_exponent");
            light.direction = aim(block, target);
        }
        return {light};
    }
    if (ciEquals(classname, "light_environment")) {
        auto sun = common(block, LightKind::Sun);
        sun.intensity = hdrColour(block, "_light", "_lightHDR", "_lightscaleHDR");
        sun.direction = aim(block, target);
        auto ambient = common(block, LightKind::Ambient);
        ambient.intensity = hdrColour(block, "_ambient", "_ambientHDR", "_AmbientScaleHDR");
        return {sun, ambient};
    }
    return {};
}

std::vector<Light> lightsFromEntity(const kv1::Node& block, const assets::EntityClass& cls,
                                    std::optional<DVec3> target) {
    auto compiled = block;
    for (auto& [key, value] : document::missingEntityDefaults(block, cls))
        compiled.append(kv1::Node::value(std::move(key), std::move(value))).value();
    return lightsFromEntity(compiled, target);
}

DVec3 irradiance(const Light& light, DVec3 point, DVec3 normal) {
    switch (light.kind) {
    case LightKind::Sun: return light.intensity * std::max(0.0, glm::dot(normal, -light.direction));
    case LightKind::Ambient: return light.intensity * (0.5 + 0.5 * normal.z);
    case LightKind::Point:
    case LightKind::Spot: break;
    }
    const DVec3 delta = light.origin - point;
    const double distance = glm::length(delta);
    if (distance <= 0 || (light.radius > 0 && distance > light.radius)) return DVec3{0.0};
    const DVec3 toLight = delta / distance;
    const double cosine = glm::dot(normal, toLight);
    if (cosine <= 0) return DVec3{0.0};
    double scale = cosine / std::max(light.attenuation(distance), 1e-12);
    if (light.kind == LightKind::Spot) {
        const double along = glm::dot(-toLight, light.direction);
        if (along <= light.stopdot2) return DVec3{0.0};
        if (along < light.stopdot) {
            double edge = (along - light.stopdot2) / (light.stopdot - light.stopdot2);
            if (light.exponent != 0 && light.exponent != 1) edge = std::pow(edge, light.exponent);
            scale *= edge;
        }
    }
    return light.intensity * scale;
}

double effectiveRange(const Light& light, double threshold) {
    constexpr double infinite = std::numeric_limits<double>::infinity();
    if (light.kind == LightKind::Sun || light.kind == LightKind::Ambient) return infinite;
    const double peak = std::max({light.intensity.x, light.intensity.y, light.intensity.z});
    if (peak <= 0) return 0;
    // Solve quadratic·d² + linear·d + constant = peak / threshold for the positive root.
    const double target = peak / threshold;
    double range = infinite;
    if (light.quadratic > 0) {
        const double disc = light.linear * light.linear - 4 * light.quadratic * (light.constant - target);
        // No real root: the attenuation never comes down to the target, so the light never reaches it.
        if (disc < 0) return 0;
        range = (-light.linear + std::sqrt(disc)) / (2 * light.quadratic);
    } else if (light.linear > 0) {
        range = (target - light.constant) / light.linear;
    } else if (light.constant >= target) {
        range = 0;
    }
    range = std::max(range, 0.0);
    return light.radius > 0 ? std::min(range, light.radius) : range;
}

} // namespace apex::lighting
