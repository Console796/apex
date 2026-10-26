#include "render/helper_shapes.hpp"

#include "assets/fgd.hpp"
#include "core/strings.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

using namespace apex;
using namespace apex::render;

namespace {

struct Fixture {
    kv1::Node block = kv1::Node::block("entity");
    assets::EntityClass cls;
    std::vector<HelperPeer> peers{{11, {10, 30, 30}}, {12, {10, 40, 30}}};

    void set(std::string key, std::string value) {
        if (auto* node = block.find(key)) REQUIRE(node->setValue(std::move(value)));
        else REQUIRE(block.append(kv1::Node::value(std::move(key), std::move(value))));
    }
    HelperContext context() const {
        return {block, cls, document::entityPose(block), {0.25f, 0.5f, 0.75f, 1},
            [this](std::string_view name) { return ciEquals(name, "destination") ? peers : std::vector<HelperPeer>{}; }};
    }
    void yawed() { set("origin", "10 20 30"); set("angles", "0 90 0"); }
    void helper(std::string name, std::string arguments = {}) { cls.helpers = {{std::move(name), std::move(arguments)}}; }
    void property(std::string name, std::string type) {
        assets::EntityProperty p;
        p.name = std::move(name);
        p.type = std::move(type);
        cls.properties.push_back(std::move(p));
    }
    HelperShapes build() const { return buildHelperShapes(context()); }
};

void point(DVec3 actual, DVec3 expected, double tolerance = 1e-8) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(tolerance).scale(1));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(tolerance).scale(1));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(tolerance).scale(1));
}
void visibility(const HelperShapes& shapes, HelperVisibility expected) {
    for (const auto& s : shapes.segments) CHECK(s.visibility == expected);
}
void finite(const HelperShapes& shapes) {
    for (const auto& s : shapes.segments) {
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(std::isfinite(s.a[axis]));
            CHECK(std::isfinite(s.b[axis]));
        }
        for (int channel = 0; channel < 4; ++channel) CHECK(std::isfinite(s.colour[channel]));
    }
    for (const auto& label : shapes.labels) {
        for (int axis = 0; axis < 3; ++axis) CHECK(std::isfinite(label.position[axis]));
        for (int channel = 0; channel < 4; ++channel) CHECK(std::isfinite(label.colour[channel]));
    }
}

} // namespace

TEST_CASE("helper shapes sphere has three world-axis great circles") {
    Fixture f;
    f.yawed();
    f.helper("sphere", "radius, 255 128 0");
    CHECK(f.build().segments.empty());
    f.set("radius", "64");
    const auto shapes = f.build();
    REQUIRE(shapes.segments.size() == 144);
    point(shapes.segments[0].a, {74, 20, 30});
    for (const auto& s : shapes.segments) {
        CHECK(glm::length(s.a - DVec3{10, 20, 30}) == doctest::Approx(64));
        CHECK(glm::length(s.b - DVec3{10, 20, 30}) == doctest::Approx(64));
        CHECK(s.colour == Vec4{1, 128.0f / 255, 0, 1});
        CHECK_FALSE(s.peer);
    }
    visibility(shapes, HelperVisibility::WhenSelected);
    for (const auto value : {"0", "-1", "nan", "inf", "1 trailing"}) {
        f.set("radius", value);
        CHECK(f.build().segments.empty());
    }
}

TEST_CASE("helper shapes line resolves all names and five-argument endpoints") {
    Fixture f;
    f.yawed();
    f.helper("line", "255 255 255, targetname, kv1_known");
    f.set("kv1_known", "DESTINATION");
    auto shapes = f.build();
    REQUIRE(shapes.segments.size() == 2);
    point(shapes.segments[0].a, {10, 20, 30});
    point(shapes.segments[0].b, {10, 30, 30});
    CHECK(shapes.segments[0].peer == 11);
    CHECK(shapes.segments[1].peer == 12);
    visibility(shapes, HelperVisibility::WhenSelfOrPeerSelected);
    f.set("kv1_known", "renamed");
    CHECK(f.build().segments.empty());
    f.set("kv1_known", "destination");
    auto noLookup = f.context();
    noLookup.peersNamed = {};
    CHECK(buildHelperShapes(noLookup).segments.empty());

    f.helper("line", "255 0 0, targetname, subject, targetname, target");
    f.set("subject", "actor");
    f.set("target", "destination");
    auto context = f.context();
    context.peersNamed = [](std::string_view name) {
        if (name == "actor") return std::vector<HelperPeer>{{21, {1, 2, 3}}};
        if (name == "destination") return std::vector<HelperPeer>{{22, {4, 5, 6}}, {23, {7, 8, 9}}};
        return std::vector<HelperPeer>{};
    };
    shapes = buildHelperShapes(context);
    REQUIRE(shapes.segments.size() == 2);
    point(shapes.segments[0].a, {1, 2, 3});
    point(shapes.segments[0].b, {4, 5, 6});
    CHECK(shapes.segments[0].peer == 22);
    f.helper("line", "255 255 255, lightingorigin, targetname");
    CHECK(f.build().segments.empty()); // Peer keyvalues are deliberately outside this API.
}

TEST_CASE("helper shapes wirebox and oriented rectangles use their specified frames") {
    Fixture f;
    f.yawed();
    f.helper("wirebox", "mins, maxs");
    f.set("mins", "-1 -2 -3");
    f.set("maxs", "4 5 6");
    auto shapes = f.build();
    REQUIRE(shapes.segments.size() == 12);
    point(shapes.segments[0].a, {9, 18, 27});
    point(shapes.segments[0].b, {14, 18, 27});
    visibility(shapes, HelperVisibility::Always);

    f.set("width", "8"); f.set("height", "4"); f.set("depth", "6");
    f.helper("orientedwidthheight", "width, height");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 4);
    point(shapes.segments[0].a, {14, 20, 28});
    point(shapes.segments[0].b, {6, 20, 28});
    visibility(shapes, HelperVisibility::WhenSelected);
    CHECK(shapes.segments[0].colour == f.context().defaultColour);
    f.helper("orientedwidthheighthalf", "width, height");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 4);
    point(shapes.segments[0].a, {18, 20, 26});
    point(shapes.segments[0].b, {2, 20, 26});

    f.helper("orientedboundingbox", "width, height, depth");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 12);
    point(shapes.segments[0].a, {14, 17, 28});
    point(shapes.segments[0].b, {14, 23, 28});
    visibility(shapes, HelperVisibility::WhenSelected);
}

TEST_CASE("helper shapes frustum uses Source angles and pitch scaling") {
    Fixture f;
    f.yawed();
    f.helper("frustum", "fov, near, far, tint, 1");
    f.set("fov", "90"); f.set("near", "2"); f.set("far", "4"); f.set("tint", "0 255 0 200");
    auto shapes = f.build();
    REQUIRE(shapes.segments.size() == 12);
    point(shapes.segments[0].a, {12, 22, 28});
    point(shapes.segments[0].b, {8, 22, 28});
    point(shapes.segments[4].a, {14, 24, 26});
    CHECK(shapes.segments[0].colour == Vec4{0, 1, 0, 1});
    visibility(shapes, HelperVisibility::WhenSelected);

    // At scale 1, reconstructed frustum axes agree with entityPose, including roll.
    f.set("angles", "30 70 20");
    shapes = f.build();
    const auto pose = document::entityPose(f.block);
    point(shapes.segments[0].a, pose.origin + pose.basis * DVec3{2, -2, -2});
    f.set("angles", "90 0 0");
    f.helper("frustum", "fov, near, far, 255 0 0, -1");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 12);
    point(shapes.segments[0].a, {12, 18, 32});
    CHECK(shapes.segments[0].colour == Vec4{1, 0, 0, 1});
}

TEST_CASE("helper shapes cylinder and lightcone rings have checkable corners") {
    Fixture f;
    f.yawed();
    f.peers.resize(1);
    f.set("target", "destination"); f.set("radius", "2");
    f.helper("cylinder", "255 255 0, targetname, target, radius");
    auto shapes = f.build();
    REQUIRE(shapes.segments.size() == 52);
    point(shapes.segments[0].a, {12, 20, 30});
    point(shapes.segments[24].a, {12, 30, 30});
    for (const auto& s : shapes.segments) CHECK(s.peer == 11);
    visibility(shapes, HelperVisibility::WhenSelected);
    f.helper("cylinder", "255 255 0, targetname, target, radius, targetname, end, endRadius");
    f.set("end", "destination"); f.set("endRadius", "4");
    auto context = f.context();
    context.peersNamed = [](std::string_view) { return std::vector<HelperPeer>{{11, {10, 30, 30}}, {12, {10, 40, 30}}}; };
    shapes = buildHelperShapes(context);
    REQUIRE(shapes.segments.size() == 104); // Coincident endpoint pairs emit nothing.
    point(shapes.segments[0].a, {12, 30, 30});
    point(shapes.segments[24].a, {14, 40, 30});

    // phys_spring/path_vphysics use an explicit self endpoint, even when unnamed.
    f.helper("cylinder", "255 255 0, targetname, target, radius, targetname, targetname, endRadius");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 52);
    point(shapes.segments[0].a, {8, 30, 30});
    point(shapes.segments[24].a, {6, 20, 30});
    for (const auto& s : shapes.segments) CHECK(s.peer == 11);

    f.helper("lightcone"); f.set("_cone", "45"); f.set("_inner_cone", "10"); f.set("_distance", "8");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 28);
    point(shapes.segments[0].a, {2, 28, 30});
    point(shapes.segments[24].a, {10, 20, 30});
    point(shapes.segments[24].b, {2, 28, 30});
    visibility(shapes, HelperVisibility::WhenSelected);
    f.property("distance", "float"); f.set("distance", "16");
    shapes = f.build();
    point(shapes.segments[0].a, {-6, 36, 30});

    Fixture absentDistance;
    absentDistance.helper("lightcone"); absentDistance.set("_cone", "45");
    shapes = absentDistance.build();
    REQUIRE(shapes.segments.size() == 28);
    point(shapes.segments[0].a, {256, 256, 0});
}

TEST_CASE("helper shapes quadbounds and swept hull use world coordinates") {
    Fixture f;
    f.yawed();
    f.helper("quadbounds");
    f.set("lowerleft", "1 2 3"); f.set("lowerright", "5 2 3");
    f.set("upperleft", "1 2 7"); f.set("upperright", "5 2 7");
    auto shapes = f.build();
    REQUIRE(shapes.segments.size() == 6);
    point(shapes.segments[0].a, {1, 2, 3}); point(shapes.segments[0].b, {5, 2, 3});
    point(shapes.segments[4].b, {5, 2, 7});
    visibility(shapes, HelperVisibility::WhenSelected);
    f.helper("sweptplayerhull");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 12);
    point(shapes.segments[0].a, {-6, 4, 30}); point(shapes.segments[0].b, {26, 4, 30});
    point(shapes.segments[2].b, {-6, 4, 102});
    visibility(shapes, HelperVisibility::WhenSelected);
    f.property("target", "target_destination"); f.set("target", "destination");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 36);
    point(shapes.segments[12].a, {-6, 14, 30});
}

TEST_CASE("helper shapes catapult low arc and minimum-speed fallback land on target") {
    Fixture f;
    f.helper("catapult"); f.set("launchTarget", "destination");
    f.peers = {{51, {600, 0, 0}}};
    // vx=600, vz=300 gives v=sqrt(450000), t=1, apex=75.
    f.set("playerSpeed", "670.8203932499369");
    auto shapes = f.build();
    REQUIRE(shapes.segments.size() == 32);
    point(shapes.segments[15].b, {300, 0, 75}, 1e-6);
    point(shapes.segments.back().b, {600, 0, 0}, 1e-6);
    visibility(shapes, HelperVisibility::WhenSelected);
    for (const auto& s : shapes.segments) CHECK(s.peer == 51);
    f.set("playerSpeed", "100");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 32);
    // Minimum speed sqrt(g*range)=600, 45 degrees, apex=range/4=150.
    point(shapes.segments[15].b, {300, 0, 150}, 1e-6);
    point(shapes.segments.back().b, {600, 0, 0}, 1e-6);
    f.peers = {{51, {0, 0, 600}}}; // Vertical targets must remain finite and land too.
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 32);
    point(shapes.segments.back().b, {0, 0, 600}, 1e-6);
    finite(shapes);
}

TEST_CASE("helper shapes catapult direction respects Source pitch and cutoff") {
    Fixture f;
    f.helper("catapult"); f.set("playerSpeed", "600"); f.set("launchDirection", "-45 0 0");
    auto shapes = f.build();
    REQUIRE(shapes.segments.size() == 32);
    double apex = 0;
    for (const auto& s : shapes.segments) apex = std::max({apex, s.a.z, s.b.z});
    // Upward 45 degrees in Source is pitch -45: 600^2*(1/2)/(2*600)=150.
    CHECK(std::abs(apex - 150) < 0.5);
    point(shapes.segments.back().b, {1896.120296218302, 0, -4096}, 1e-6);
    // This launch reaches the floor before five seconds, so its endpoint is z=-4096.
    CHECK(shapes.segments.back().b.z == doctest::Approx(-4096));
    f.set("launchDirection", "45 0 0");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 32);
    CHECK(shapes.segments[0].b.z < 0);
    CHECK(shapes.segments.back().b.z == doctest::Approx(-4096));
    f.set("launchDirection", "-90 0 0"); f.set("playerSpeed", "1000");
    shapes = f.build();
    REQUIRE(shapes.segments.size() == 32);
    point(shapes.segments.back().b, {0, 0, -2500}, 1e-6); // Five-second cutoff.
}

TEST_CASE("helper shapes chains sidelist worldtext and unsupported helpers") {
    Fixture f;
    f.yawed();
    f.property("NextKey", "target_destination"); f.set("nextkey", "destination");
    for (const auto name : {"keyframe", "animator"}) {
        f.helper(name);
        const auto shapes = f.build();
        REQUIRE(shapes.segments.size() == 2);
        point(shapes.segments[0].a, {10, 20, 30}); point(shapes.segments[0].b, {10, 30, 30});
        CHECK(shapes.segments[0].peer == 11);
        visibility(shapes, HelperVisibility::Always);
    }
    f.cls.properties.clear();
    CHECK(f.build().segments.empty());
    f.property("sides", "sidelist"); f.property("sides2", "SIDELIST");
    f.set("sides", "1 2\t3 2 nope 4x 2147483648 +5"); f.set("sides2", "3\n6 -7");
    f.helper("sidelist");
    auto shapes = f.build();
    CHECK(shapes.segments.empty());
    CHECK(shapes.sideIds == std::vector<int>{1, 2, 3, 5, 6, -7});
    f.helper("worldtext"); f.set("message", "Hello, world");
    shapes = f.build();
    REQUIRE(shapes.labels.size() == 1);
    point(shapes.labels[0].position, {10, 20, 30});
    CHECK(shapes.labels[0].text == "Hello, world");
    Fixture fallback;
    fallback.cls.name = "point_worldtext"; fallback.helper("worldtext");
    CHECK(fallback.build().labels[0].text == "point_worldtext");
    fallback.property("textstring", "string"); fallback.set("textstring", "Alternate");
    CHECK(fallback.build().labels[0].text == "Alternate");

    f.cls.helpers.clear();
    for (const auto name : {"size", "bbox", "studio", "studioprop", "lightprop", "iconsprite", "sprite", "color", "instance",
                            "light", "clusteredlight", "decal", "overlay", "overlay_transition", "Mystery", "LIGHT", "mystery"})
        f.cls.helpers.push_back({name, {}});
    shapes = f.build();
    CHECK(shapes.segments.empty()); CHECK(shapes.labels.empty());
    CHECK(shapes.unsupported == std::vector<std::string>{"light", "clusteredlight", "decal", "overlay", "overlay_transition", "mystery"});
}

TEST_CASE("helper shapes malformed keys and nonfinite inputs never emit geometry") {
    Fixture f;
    f.cls.helpers = {{"sphere", "radius"}, {"wirebox", "mins,maxs"}, {"frustum", "fov,near,far"},
        {"cylinder", "255 255 255,targetname,target,radius"}, {"lightcone", ""},
        {"orientedwidthheight", "width,height"}, {"orientedboundingbox", "width,height,depth"},
        {"quadbounds", ""}, {"catapult", ""}};
    for (const auto value : {"garbage", "nan", "inf", "1e999", "1 trailing"}) {
        for (const auto key : {"radius", "mins", "maxs", "fov", "near", "far", "_cone", "_distance",
                               "width", "height", "depth", "lowerleft", "lowerright", "upperleft", "upperright",
                               "playerSpeed", "launchDirection"}) f.set(key, value);
        f.set("target", "destination");
        const auto shapes = f.build();
        CHECK(shapes.segments.empty());
        finite(shapes);
    }
    f.helper("sphere", "radius"); f.set("radius", "1e308"); f.set("origin", "1e308 0 0");
    CHECK(f.build().segments.empty()); // Overflow discards the entire helper.
    f.set("radius", "64");
    auto context = f.context();
    context.pose.origin.x = std::numeric_limits<double>::quiet_NaN();
    CHECK(buildHelperShapes(context).segments.empty());
}

TEST_CASE("helper shapes empty-block P2CE FGD corpus remains finite") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    const auto table = assets::loadEntityClasses(std::filesystem::path(install) / "p2ce/p2ce.fgd");
    REQUIRE_MESSAGE(table, (table ? "" : table.error().what()));
    REQUIRE(table->classes.size() > 100);
    MESSAGE("Checking helper shapes for ", table->classes.size(), " P2CE classes");
    const auto block = kv1::Node::block("entity");
    for (const auto& [name, cls] : table->classes) {
        INFO(name);
        HelperShapes shapes;
        REQUIRE_NOTHROW(shapes = buildHelperShapes({block, cls, {}, {1, 1, 1, 1}, {}}));
        finite(shapes);
    }
}
