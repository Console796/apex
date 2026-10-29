#include "lighting/light.hpp"

#include "core/json.hpp"
#include "core/kv1.hpp"
#include "document/entity_helpers.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string_view>

using namespace apex;

namespace {

std::string fixtureText(std::string_view name = "worldlights.json") {
    std::ifstream stream(std::string(APEX_FIXTURE_DIR) + "/lighting/" + std::string(name), std::ios::binary);
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

const json::Value& field(const json::Value& object, std::string_view key) {
    static const json::Value missing;
    const auto* map = object.get<json::Value::Object>();
    const auto it = map ? map->find(key) : decltype(map->end()){};
    return map && it != map->end() ? it->second : missing;
}
double num(const json::Value& object, std::string_view key) {
    const auto* value = field(object, key).get<double>();
    REQUIRE(value);
    return *value;
}
DVec3 vec(const json::Value& object, std::string_view key) {
    const auto* array = field(object, key).get<json::Value::Array>();
    REQUIRE(array);
    REQUIRE(array->size() == 3);
    return {*(*array)[0].get<double>(), *(*array)[1].get<double>(), *(*array)[2].get<double>()};
}
std::string str(const json::Value& object, std::string_view key) {
    const auto* value = field(object, key).get<std::string>();
    return value ? *value : std::string{};
}

kv1::Node entity(const json::Value& testCase) {
    const auto origin = vec(testCase, "origin");
    std::string text = "entity { classname \"" + str(testCase, "classname") + "\" origin \"" +
                       std::to_string(origin.x) + " " + std::to_string(origin.y) + " " + std::to_string(origin.z) + "\"";
    for (const auto& [key, value] : *field(testCase, "keys").get<json::Value::Object>())
        text += " \"" + key + "\" \"" + *value.get<std::string>() + "\"";
    auto parsed = kv1::parse(text + " }");
    REQUIRE(parsed);
    return *parsed->root().find("entity");
}

lighting::LightKind kindOf(const std::string& type) {
    if (type == "point") return lighting::LightKind::Point;
    if (type == "spot") return lighting::LightKind::Spot;
    if (type == "skylight") return lighting::LightKind::Sun;
    if (type == "skyambient") return lighting::LightKind::Ambient;
    FAIL("unexpected worldlight type " << type);
    return {};
}

// The lump stores float32: compare relative to magnitude, with a floor for values near zero.
bool near(double a, double b, double relative = 2e-6, double floor = 1e-9) {
    return std::abs(a - b) <= relative * std::max(std::abs(a), std::abs(b)) + floor;
}
bool nearVec(DVec3 a, DVec3 b, double relative = 2e-6, double floor = 1e-6) {
    return near(a.x, b.x, relative, floor) && near(a.y, b.y, relative, floor) && near(a.z, b.z, relative, floor);
}

// The 50%/0% solve is reproduced through float arithmetic in VRAD, so compare the curves it gives.
bool sameCurve(const lighting::Light& light, const json::Value& record, double extent) {
    for (int i = 1; i <= 100; ++i) {
        const double d = extent * i / 100;
        const double measured = num(record, "constant") + num(record, "linear") * d + num(record, "quadratic") * d * d;
        if (std::abs(light.attenuation(d) - measured) > 1e-4 * std::abs(measured)) return false;
    }
    return true;
}

} // namespace

TEST_CASE("light model reproduces every native VRAD worldlight measured by the 0.6 probe") {
    auto root = json::parse(fixtureText());
    REQUIRE(root);
    const auto* cases = field(*root, "cases").get<json::Value::Array>();
    REQUIRE(cases);
    REQUIRE(cases->size() > 400);
    std::size_t records = 0;
    for (const auto& testCase : *cases) {
        INFO(str(testCase, "classname") << " / " << str(testCase, "label"));
        const auto block = entity(testCase);
        const auto lights = lighting::lightsFromEntity(block);
        const auto& expected = *field(testCase, "worldlights").get<json::Value::Array>();
        REQUIRE(lights.size() == expected.size());
        for (const auto& record : expected) {
            const auto kind = kindOf(str(record, "type"));
            const auto it = std::ranges::find(lights, kind, &lighting::Light::kind);
            REQUIRE(it != lights.end());
            const auto& light = *it;
            ++records;
            CHECK(nearVec(light.intensity, vec(record, "intensity"), 2e-6, 1e-7));
            CHECK(nearVec(light.shadowCastOffset, vec(record, "shadow_cast_offset")));
            // The probe matches records to inputs by origin, so the input origin is the measured one.
            CHECK(nearVec(light.origin, vec(testCase, "origin")));
            if (kind == lighting::LightKind::Spot || kind == lighting::LightKind::Sun)
                CHECK(nearVec(light.direction, vec(record, "normal"), 1e-5, 1e-6));
            // VRAD takes cosines in float32.
            CHECK(near(light.stopdot, num(record, "stopdot"), 2e-6, 1e-7));
            CHECK(near(light.stopdot2, num(record, "stopdot2"), 2e-6, 1e-7));
            CHECK(light.exponent == num(record, "exponent"));
            CHECK(light.radius == num(record, "radius"));
            const auto& keys = field(testCase, "keys");
            if (!str(keys, "_fifty_percent_distance").empty() && kind != lighting::LightKind::Sun) {
                const double zero = std::max(std::atof(str(keys, "_zero_percent_distance").c_str()),
                                             2 * std::atof(str(keys, "_fifty_percent_distance").c_str()));
                // l and q differ from the lump by up to 1e-3 relative where the solve cancels (VRAD's float32
                // operation order is not reproducible); the constant and the curve are exact.
                CHECK(near(light.constant, num(record, "constant"), 2e-6));
                CHECK(sameCurve(light, record, 1.2 * zero));
            } else {
                CHECK(near(light.constant, num(record, "constant")));
                CHECK(near(light.linear, num(record, "linear")));
                CHECK(near(light.quadratic, num(record, "quadratic")));
            }
            // A named light's style is numbered across the whole map; its name is what is shared.
            if (light.styleName.empty()) CHECK(light.style == num(record, "style"));
            CHECK(light.castEntityShadow == ((static_cast<int>(num(record, "flags")) & 2) != 0));
            CHECK(light.specularMode == num(record, "specular_mode"));
            CHECK(light.directMode == num(record, "direct_mode"));
            CHECK(light.indirectMode == num(record, "indirect_mode"));
        }
    }
    CHECK(records > 400);
}

TEST_CASE("missing keys mean FGD defaults, exactly as the compile snapshot writes them") {
    auto parsed = kv1::parse(R"(entity { classname "light_spot" origin "0 0 0" })");
    REQUIRE(parsed);
    const auto& bare = *parsed->root().find("entity");
    assets::EntityClass cls;
    cls.name = "light_spot";
    for (const auto& [name, value] : {std::pair{"_light", "255 255 255 200"}, {"_inner_cone", "30"}, {"_cone", "45"},
                                      {"_exponent", "1"}, {"pitch", "-90"}, {"_quadratic_attn", "1"}}) {
        assets::EntityProperty property;
        property.name = name;
        property.defaultValue = value;
        cls.properties.push_back(property);
    }
    auto written = bare;
    for (const auto& [key, value] : document::missingEntityDefaults(bare, cls))
        REQUIRE(written.append(kv1::Node::value(key, value)));
    const auto fromDefaults = lighting::lightsFromEntity(bare, cls);
    const auto fromWritten = lighting::lightsFromEntity(written);
    REQUIRE(fromDefaults.size() == 1);
    REQUIRE(fromWritten.size() == 1);
    CHECK(fromDefaults[0].intensity == fromWritten[0].intensity);
    CHECK(fromDefaults[0].exponent == 1);
    CHECK(fromDefaults[0].stopdot2 == fromWritten[0].stopdot2);
    // Without the FGD, VRAD's own fallbacks apply: exponent 0 and a 10-degree cone.
    const auto raw = lighting::lightsFromEntity(bare);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].exponent == 0);
    CHECK(raw[0].intensity == DVec3{0.0});
}

TEST_CASE("fifty/zero attenuation puts 50% at the fifty distance and 1/256 of the source at zero") {
    for (const double fifty : {64.0, 100.0, 256.0, 1000.0})
        for (const double ratio : {1.25, 2.0, 3.0, 4.0, 7.5, 10.0, 15.0, 20.0, 100.0}) {
            CAPTURE(fifty);
            CAPTURE(ratio);
            const auto a = lighting::fiftyZeroAttenuation(fifty, fifty * ratio);
            const double zero = fifty * ratio;
            CHECK(a.constant + a.linear * fifty + a.quadratic * fifty * fifty == doctest::Approx(2.0));
            CHECK((a.constant + a.linear * zero + a.quadratic * zero * zero) / a.constant == doctest::Approx(256.0));
            CHECK(a.linear + a.quadratic >= 0);
        }
    // A zero distance at or inside the fifty distance means twice it.
    const auto implied = lighting::fiftyZeroAttenuation(100, 0);
    const auto doubled = lighting::fiftyZeroAttenuation(100, 200);
    CHECK(implied.constant == doubled.constant);
    CHECK(implied.quadratic == doubled.quadratic);
    const auto equal = lighting::fiftyZeroAttenuation(100, 100);
    CHECK(equal.constant == 0);
    CHECK(equal.linear == doctest::Approx(0.02));
    CHECK(equal.quadratic == 0);
}

TEST_CASE("irradiance follows the inverse attenuation, VRAD's surface response and the cones; range bounds it") {
    lighting::Light light;
    light.kind = lighting::LightKind::Point;
    light.origin = {0, 0, 100};
    light.intensity = DVec3{10000.0};
    light.quadratic = 1;
    // Straight below at 100 units: intensity / 100².
    CHECK(lighting::irradiance(light, {0, 0, 0}, {0, 0, 1}).x == doctest::Approx(1.0));
    CHECK(lighting::irradiance(light, {0, 0, 0}, {0, 0, -1}).x == 0);
    // Measured (lighting_0_6_2_reference.md): the response to light at cosine c is (c + c²)/2, not c.
    const double c45 = std::sqrt(0.5);
    CHECK(lighting::irradiance(light, {100, 0, 0}, {0, 0, 1}).x == doctest::Approx(10000.0 / 20000.0 * 0.5 * c45 * (1 + c45)));
    light.radius = 50;
    CHECK(lighting::irradiance(light, {0, 0, 0}, {0, 0, 1}).x == 0);
    light.radius = 0;
    CHECK(lighting::effectiveRange(light, 1.0) == doctest::Approx(100.0));
    light.radius = 60;
    CHECK(lighting::effectiveRange(light, 1.0) == doctest::Approx(60.0));

    light.kind = lighting::LightKind::Spot;
    light.radius = 0;
    light.direction = {0, 0, -1};
    light.stopdot = std::cos(glm::radians(30.0));
    light.stopdot2 = std::cos(glm::radians(45.0));
    CHECK(lighting::irradiance(light, {0, 0, 0}, {0, 0, 1}).x == doctest::Approx(1.0));
    CHECK(lighting::irradiance(light, {200, 0, 0}, {0, 0, 1}).x == 0); // 63° off axis
    const double edge = lighting::irradiance(light, {std::tan(glm::radians(37.5)) * 100, 0, 0}, {0, 0, 1}).x;
    CHECK(edge > 0);
    CHECK(edge < 1.0);
    // Inside the inner cone the spot is a cosine emitter: the axis cosine multiplies the surface response.
    const double c20 = std::cos(glm::radians(20.0));
    const DVec3 at20{std::tan(glm::radians(20.0)) * 100, 0, 0};
    CHECK(lighting::irradiance(light, at20, {0, 0, 1}).x ==
          doctest::Approx(10000.0 / (100 * 100 / (c20 * c20)) * 0.5 * c20 * (1 + c20) * c20));
    // Between the cones: the linear ramp, raised to the exponent unless the exponent is 0 or 1.
    const double c375 = std::cos(glm::radians(37.5));
    const double ramp = (c375 - light.stopdot2) / (light.stopdot - light.stopdot2);
    const double base = 10000.0 / (100 * 100 / (c375 * c375)) * 0.5 * c375 * (1 + c375) * c375;
    CHECK(edge == doctest::Approx(base * ramp));
    light.exponent = 1;
    CHECK(lighting::irradiance(light, {std::tan(glm::radians(37.5)) * 100, 0, 0}, {0, 0, 1}).x == doctest::Approx(base * ramp));
    light.exponent = 3;
    CHECK(lighting::irradiance(light, {std::tan(glm::radians(37.5)) * 100, 0, 0}, {0, 0, 1}).x ==
          doctest::Approx(base * ramp * ramp * ramp));
    light.exponent = 0;

    lighting::Light sun;
    sun.kind = lighting::LightKind::Sun;
    sun.intensity = DVec3{2.0};
    sun.direction = {0, -std::sin(glm::radians(60.0)), -std::cos(glm::radians(60.0))};
    CHECK(lighting::irradiance(sun, {}, {0, 0, 1}).x == doctest::Approx(2.0 * 0.5 * 0.5 * 1.5));
    CHECK(lighting::irradiance(sun, {}, {0, 0, -1}).x == 0);

    // Sky ambient under an open upper hemisphere, weighted by the same response: 1 up, ½ vertical, 0 down;
    // a 60° slope is 0.77179 (measured 0.7718 by quadrature, not the 0.75 a cosine weighting gives).
    lighting::Light ambient;
    ambient.kind = lighting::LightKind::Ambient;
    ambient.intensity = DVec3{1.0};
    CHECK(lighting::irradiance(ambient, {}, {0, 0, 1}).x == doctest::Approx(1.0));
    CHECK(lighting::irradiance(ambient, {}, {1, 0, 0}).x == doctest::Approx(0.5));
    CHECK(lighting::irradiance(ambient, {}, {0, 0, -1}).x == doctest::Approx(0.0));
    CHECK(lighting::irradiance(ambient, {}, {std::sqrt(0.75), 0, 0.5}).x == doctest::Approx(0.77179).epsilon(1e-4));

    // A curve whose minimum stays above peak/threshold never reaches the threshold.
    lighting::Light dim = light;
    dim.kind = lighting::LightKind::Point;
    dim.intensity = DVec3{100.0};
    dim.constant = 1;
    dim.linear = -0.1;
    dim.quadratic = 0.01; // minimum 0.75 at d = 5, so peak 100 never exceeds 134
    CHECK(lighting::effectiveRange(dim, 200.0) == 0);

    lighting::Light constant;
    constant.intensity = DVec3{1.0};
    constant.constant = 1;
    CHECK(std::isinf(lighting::effectiveRange(constant, 0.5)));
    CHECK(lighting::effectiveRange(constant, 2.0) == 0);
}

// D3 test 3 without the native tools: luxels of native VRAD -bounce 0 -noextra -aoscale 0 lightmaps, recorded by
// tools/light_parity.py --fixture, against the model averaged over VRAD's measured four sample points.
TEST_CASE("irradiance reproduces native VRAD's direct lightmaps on the parity reference scenes") {
    const auto parsed = json::parse(fixtureText("irradiance_parity.json"));
    REQUIRE(parsed);
    const auto* scenes = field(*parsed, "scenes").get<json::Value::Object>();
    REQUIRE(scenes);
    REQUIRE(scenes->size() >= 6);
    for (const auto& [name, scene] : *scenes) {
        CAPTURE(name);
        std::vector<lighting::Light> lights;
        for (const auto& record : *field(scene, "lights").get<json::Value::Array>()) {
            lighting::Light light;
            const int type = static_cast<int>(num(record, "type"));
            light.kind = type == 1 ? lighting::LightKind::Point : type == 2 ? lighting::LightKind::Spot
                                                                            : lighting::LightKind::Sun;
            light.origin = vec(record, "origin");
            light.intensity = vec(record, "intensity");
            if (type != 1) light.direction = vec(record, "normal");
            light.stopdot = num(record, "stopdot");
            light.stopdot2 = num(record, "stopdot2");
            light.exponent = num(record, "exponent");
            light.radius = num(record, "radius");
            light.constant = num(record, "constant");
            light.linear = num(record, "linear");
            light.quadratic = num(record, "quadratic");
            lights.push_back(light);
        }
        const auto& samples = *field(scene, "samples").get<json::Value::Array>();
        REQUIRE(samples.size() >= 16);
        double worst = 0;
        for (const auto& sample : samples) {
            const DVec3 normal = vec(sample, "normal");
            const DVec3 reference = vec(sample, "reference");
            DVec3 model{0.0};
            const auto& points = *field(sample, "points").get<json::Value::Array>();
            for (const auto& point : points) {
                const auto& xyz = *point.get<json::Value::Array>();
                const DVec3 p{*xyz[0].get<double>(), *xyz[1].get<double>(), *xyz[2].get<double>()};
                for (const auto& light : lights) model += lighting::irradiance(light, p, normal);
            }
            model /= static_cast<double>(points.size());
            for (int i = 0; i < 3; ++i) {
                // The lightmap keeps an 8-bit mantissa per channel, so dim channels of a coloured luxel carry
                // more quantization; judge each against the luxel's brightest channel.
                const double scale = std::max({reference.x, reference.y, reference.z});
                worst = std::max(worst, std::abs(model[i] - reference[i]) / scale);
            }
        }
        CHECK(worst < 0.01);
    }
}
