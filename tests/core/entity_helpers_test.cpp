#include "core/entity_helpers.hpp"

#include <doctest/doctest.h>

using namespace apex::assets;

namespace {
template <typename T> T helper(std::string name, std::string arguments = {}) {
    auto parsed = parseHelper({std::move(name), std::move(arguments)});
    REQUIRE(std::holds_alternative<T>(parsed));
    return std::get<T>(parsed);
}
template <typename T> void tag(std::string name) {
    helper<T>(name);
    helper<T>(name, "malformed, (argument");
}
}

TEST_CASE("helper arguments split commas outside quotes and parentheses") {
    CHECK(splitHelperArguments(R"args( a, "models/a,b(c).mdl", (b, (c, d)), "" )args") ==
        std::vector<std::string>{"a", "models/a,b(c).mdl", "(b, (c, d))", ""});
    CHECK(splitHelperArguments(" \t").empty());
    CHECK(splitHelperArguments(",a,") == std::vector<std::string>{"", "a", ""});
    CHECK(splitHelperArguments(R"args("a\",b", c)args") ==
        std::vector<std::string>{R"args(a\",b)args", "c"});
    CHECK(splitHelperArguments(R"args("a" "b", c)args") ==
        std::vector<std::string>{R"args("a" "b")args", "c"});
    CHECK(splitHelperArguments("a, (b, c") == std::vector<std::string>{"a", "(b, c"});
    CHECK(splitHelperArguments("a), b") == std::vector<std::string>{"a)", "b"});
    CHECK(splitHelperArguments("\"a, b") == std::vector<std::string>{"\"a, b"});
}

TEST_CASE("helper sphere and line parse keys colours and endpoint pairs") {
    auto sphere = helper<SphereHelper>("SpHeRe", "radius, 255 128 0");
    CHECK(sphere.radiusKey == "radius");
    CHECK(sphere.colour == std::array<int, 3>{255, 128, 0});
    CHECK(helper<SphereHelper>("sphere").radiusKey.empty());
    CHECK_FALSE(helper<SphereHelper>("sphere", "radius, red").colour);
    CHECK_FALSE(helper<SphereHelper>("sphere", "radius, 1 2 3 extra").colour);
    CHECK_FALSE(helper<SphereHelper>("sphere", "radius, 999999999999 2 3").colour);
    auto line = helper<LineHelper>("line", "255 255 255, targetname, kv1_known");
    CHECK(line.colour == std::array<int, 3>{255, 255, 255});
    CHECK(line.startKey == "targetname");
    CHECK(line.startValue == "kv1_known");
    CHECK(line.endKey.empty());
    CHECK(line.endValue.empty());
    line = helper<LineHelper>("line", "255 0 0, targetname, target, nodeid, target2");
    CHECK(line.startValue == "target");
    CHECK(line.endKey == "nodeid");
    CHECK(line.endValue == "target2");
    CHECK(helper<LineHelper>("line").startKey.empty());
    CHECK(helper<LineHelper>("line", "255 0 0, targetname, (broken").startKey.empty());
    CHECK_FALSE(helper<LineHelper>("line", "1 2, targetname").colour);
}

TEST_CASE("helper models sprites boxes and colour retain typed values") {
    CHECK(helper<StudioHelper>("studio", R"("models/a,b(c).mdl")").model == "models/a,b(c).mdl");
    CHECK(helper<StudioPropHelper>("studioprop", R"("models/prop.mdl")").model == "models/prop.mdl");
    CHECK(helper<LightPropHelper>("lightprop", R"("models/editor/spot.mdl")").model == "models/editor/spot.mdl");
    CHECK(helper<IconSpriteHelper>("iconsprite", R"("editor/a.vmt")").material == "editor/a.vmt");
    CHECK(helper<StudioHelper>("studio").model.empty());
    CHECK(helper<StudioHelper>("studio", "\"unterminated").model.empty());
    CHECK(helper<StudioPropHelper>("studioprop").model.empty());
    CHECK(helper<LightPropHelper>("lightprop").model.empty());
    CHECK(helper<IconSpriteHelper>("iconsprite").material.empty());
    auto box = helper<WireboxHelper>("wirebox", "mins, maxs");
    CHECK(box.minsKey == "mins");
    CHECK(box.maxsKey == "maxs");
    CHECK(helper<WireboxHelper>("wirebox").minsKey.empty());
    CHECK(helper<WireboxHelper>("wirebox", "mins").maxsKey.empty());
    auto size = helper<SizeHelper>("size", "8 16 32");
    CHECK(size.mins == std::array<double, 3>{-4, -8, -16});
    CHECK(size.maxs == std::array<double, 3>{4, 8, 16});
    size = helper<SizeHelper>("size", "-1 -2 -3, 4 5 6");
    CHECK(size.mins == std::array<double, 3>{-1, -2, -3});
    CHECK(size.maxs == std::array<double, 3>{4, 5, 6});
    auto bbox = helper<BboxHelper>("bbox", "-2 -3 -4, 5 6 7");
    CHECK(bbox.mins == std::array<double, 3>{-2, -3, -4});
    CHECK(bbox.maxs == std::array<double, 3>{5, 6, 7});
    CHECK_FALSE(helper<SizeHelper>("size").mins);
    CHECK_FALSE(helper<SizeHelper>("size", "bad, worse").maxs);
    CHECK_FALSE(helper<BboxHelper>("bbox").maxs);
    CHECK_FALSE(helper<BboxHelper>("bbox", "1 2").mins);
    CHECK(helper<ColorHelper>("color", "12 34 56").colour == std::array<int, 3>{12, 34, 56});
    CHECK_FALSE(helper<ColorHelper>("color").colour);
    CHECK_FALSE(helper<ColorHelper>("color", "12 34 nope").colour);
}

TEST_CASE("helper frustum cylinder and oriented bounds parse corpus forms") {
    auto frustum = helper<FrustumHelper>("frustum", "lightfov, nearz, farz, lightcolor, -1");
    CHECK(frustum.fovKey == "lightfov");
    CHECK(frustum.nearKey == "nearz");
    CHECK(frustum.farKey == "farz");
    CHECK(frustum.colourKey == "lightcolor");
    CHECK_FALSE(frustum.colour);
    CHECK(frustum.pitchScale == -1);
    CHECK(helper<FrustumHelper>("frustum", "fov, near, far, 12 34 56").colour ==
        std::array<int, 3>{12, 34, 56});
    CHECK(helper<FrustumHelper>("frustum").fovKey.empty());
    CHECK(helper<FrustumHelper>("frustum", ",,,,garbage").pitchScale == 1);
    CHECK(helper<FrustumHelper>("frustum", ",,,,2junk").pitchScale == 1);
    auto cylinder = helper<CylinderHelper>("cylinder", "255 255 255, targetname, targetref, radius");
    CHECK(cylinder.colour == std::array<int, 3>{255, 255, 255});
    CHECK(cylinder.startKey == "targetname");
    CHECK(cylinder.startValue == "targetref");
    CHECK(cylinder.startRadiusKey == "radius");
    CHECK(cylinder.endKey.empty());
    cylinder = helper<CylinderHelper>("cylinder", "255 0 0, targetname, targetname, radius, targetname, targetentityname");
    CHECK(cylinder.endKey == "targetname");
    CHECK(cylinder.endValue == "targetentityname");
    CHECK(cylinder.endRadiusKey.empty());
    cylinder = helper<CylinderHelper>("cylinder", "255 255 255, targetname, LightningStart, Radius, targetname, LightningEnd, Radius");
    CHECK(cylinder.endValue == "LightningEnd");
    CHECK(cylinder.endRadiusKey == "Radius");
    CHECK(helper<CylinderHelper>("cylinder").startRadiusKey.empty());
    CHECK_FALSE(helper<CylinderHelper>("cylinder", "bad").colour);
    auto oriented = helper<OrientedWidthHeightHelper>("orientedwidthheight", "width, height");
    CHECK(oriented.widthKey == "width");
    CHECK(oriented.heightKey == "height");
    CHECK_FALSE(oriented.half);
    CHECK(helper<OrientedWidthHeightHelper>("orientedwidthheighthalf", "width, height").half);
    CHECK(helper<OrientedWidthHeightHelper>("orientedwidthheight").widthKey.empty());
    CHECK(helper<OrientedWidthHeightHelper>("orientedwidthheighthalf", "width").heightKey.empty());
    auto obb = helper<OrientedBoundingBoxHelper>("orientedboundingbox", "width, height, depth");
    CHECK(obb.widthKey == "width");
    CHECK(obb.heightKey == "height");
    CHECK(obb.depthKey == "depth");
    CHECK(helper<OrientedBoundingBoxHelper>("orientedboundingbox").depthKey.empty());
    CHECK(helper<OrientedBoundingBoxHelper>("orientedboundingbox", "width").heightKey.empty());
    CHECK(helper<ClusteredLightHelper>("clusteredlight", "point").kind == "point");
    CHECK(helper<ClusteredLightHelper>("clusteredlight", "spot").kind == "spot");
    CHECK(helper<ClusteredLightHelper>("clusteredlight").kind.empty());
}

TEST_CASE("helper tags and unknown helpers survive empty and malformed arguments") {
    tag<CatapultHelper>("catapult");
    tag<KeyframeHelper>("keyframe");
    tag<AnimatorHelper>("animator");
    tag<SidelistHelper>("sidelist");
    tag<QuadboundsHelper>("quadbounds");
    tag<SweptPlayerHullHelper>("sweptplayerhull");
    tag<LightconeHelper>("lightcone");
    tag<WorldtextHelper>("worldtext");
    tag<InstanceHelper>("instance");
    tag<SpriteHelper>("sprite");
    tag<DecalHelper>("decal");
    tag<OverlayHelper>("overlay");
    tag<OverlayTransitionHelper>("overlay_transition");
    tag<LightHelper>("light");
    const auto unknown = helper<UnknownHelper>("Foo", "a, (b, c)");
    CHECK(unknown.name == "Foo");
    CHECK(unknown.arguments == "a, (b, c)");
    CHECK(helper<UnknownHelper>("future").arguments.empty());
}

TEST_CASE("helper malformed arguments never change a recognised kind into unknown") {
    for (const auto* name : {"sphere", "line", "studio", "studioprop", "lightprop", "iconsprite",
         "wirebox", "frustum", "cylinder", "orientedwidthheight", "orientedwidthheighthalf",
         "orientedboundingbox", "clusteredlight", "size", "bbox", "color"}) {
        INFO(name);
        const auto empty = parseHelper({name, ""});
        CHECK_FALSE(std::holds_alternative<UnknownHelper>(empty));
        for (const auto* arguments : {"\"unterminated, (", ",,", "(nested, bad", "1 2 nope"}) {
            CHECK(parseHelper({name, arguments}).index() == empty.index());
        }
    }
}
