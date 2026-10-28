#include "document/entity_transform.hpp"

#include "core/strings.hpp"

#include <doctest/doctest.h>

#include <fstream>
#include <iomanip>
#include <sstream>

using namespace apex;

namespace {

void checkVector(DVec3 actual, DVec3 expected) {
    for (int axis = 0; axis < 3; ++axis)
        CHECK(std::abs(actual[axis] - expected[axis]) < 1e-9);
}

kv1::Node entity(std::string_view classname, std::string_view uniform, std::string_view model) {
    auto block = kv1::Node::block("entity");
    block.append(kv1::Node::value("classname", std::string(classname))).value();
    if (!uniform.empty()) block.append(kv1::Node::value("uniformscale", std::string(uniform))).value();
    if (!model.empty()) block.append(kv1::Node::value("modelscale", std::string(model))).value();
    return block;
}

} // namespace

TEST_CASE("entity transform matches srctools angle grid") {
    std::ifstream fixture(std::string(APEX_FIXTURE_DIR) + "/entity_angles_srctools.txt");
    REQUIRE(fixture.is_open());
    std::string line;
    std::size_t cases = 0;
    while (std::getline(fixture, line)) {
        if (line.starts_with('#')) continue;
        CAPTURE(line);
        std::istringstream values(line);
        DVec3 angles, forward, left, up;
        REQUIRE(static_cast<bool>(values >> angles.x >> angles.y >> angles.z
                                        >> forward.x >> forward.y >> forward.z
                                        >> left.x >> left.y >> left.z >> up.x >> up.y >> up.z));
        std::ostringstream text;
        text << std::setprecision(17) << angles.x << ' ' << angles.y << ' ' << angles.z;
        auto block = entity("prop_static", "2.5", "7");
        block.append(kv1::Node::value("angles", text.str())).value();
        block.append(kv1::Node::value("origin", "100 -20 3.5")).value();
        const auto pose = document::entityPose(block);
        checkVector(pose.anglesDeg, angles);
        checkVector(pose.basis[0], forward);
        checkVector(pose.basis[1], left);
        CHECK(glm::determinant(pose.basis) == doctest::Approx(1.0).epsilon(1e-9));
        checkVector(pose.basis[2], up);
        const DVec3 point{3, -5, 7};
        checkVector(DVec3(document::entityTransform(pose) * DVec4(point, 1.0)),
                    pose.origin + pose.basis * point * pose.scale);
        ++cases;
    }
    CHECK(fixture.eof());
    CHECK(cases == 3377);
}

TEST_CASE("entity transform missing and malformed keys use defaults") {
    auto block = kv1::Node::block("entity");
    for (const bool malformed : {false, true}) {
        if (malformed) {
            block.append(kv1::Node::value("origin", "bad vector")).value();
            block.append(kv1::Node::value("angles", "1 2")).value();
        }
        const auto pose = document::entityPose(block);
        checkVector(pose.origin, DVec3{0.0});
        checkVector(pose.anglesDeg, DVec3{0.0});
        CHECK(pose.scale == 1.0);
        for (int column = 0; column < 3; ++column) checkVector(pose.basis[column], DMat3{1.0}[column]);
        const DVec4 point{2, -3, 4, 1};
        CHECK(document::entityTransform(pose) * point == point);
    }
}

TEST_CASE("entity transform scale preference fallbacks and invalid values") {
    for (const auto classname : {"prop_static", "PROP_STATIC", "prop_dynamic", "light", ""}) {
        CAPTURE(classname);
        const bool isStatic = ciEquals(classname, "prop_static");
        CHECK(document::entityPose(entity(classname, "2", "3")).scale == (isStatic ? 2.0 : 3.0));
        CHECK(document::entityPose(entity(classname, "2", "")).scale == 2.0);
        CHECK(document::entityPose(entity(classname, "", "3")).scale == 3.0);
        CHECK(document::entityPose(entity(classname, "", "")).scale == 1.0);
        for (const auto bad : {"bad", "0", "-2", "nan", "inf", "-inf", "1e999", "1 2 3"}) {
            CAPTURE(bad);
            CHECK(document::entityPose(entity(classname, isStatic ? bad : "2", isStatic ? "3" : bad)).scale == 1.0);
        }
    }
}

TEST_CASE("entity transform origin uses KV1 vector parsing") {
    for (const auto origin : {"1.5 -2 3e2", "[1.5 -2 300]"}) {
        auto block = kv1::Node::block("entity");
        block.append(kv1::Node::value("origin", origin)).value();
        const auto pose = document::entityPose(block);
        checkVector(pose.origin, {1.5, -2, 300});
        checkVector(DVec3(document::entityTransform(pose) * DVec4{0, 0, 0, 1}), pose.origin);
    }
    for (const auto origin : {"", "1 2", "1 2 3 4", "a b c"}) {
        auto block = kv1::Node::block("entity");
        block.append(kv1::Node::value("origin", origin)).value();
        checkVector(document::entityPose(block).origin, DVec3{0.0});
    }
}
