#include "lighting/bake_casters.hpp"

#include "document/document.hpp"
#include "geometry/solid_block.hpp"

#include <doctest/doctest.h>

using namespace apex;
using namespace apex::lighting;

TEST_CASE("bake casters: two brushes retain sky, nodraw and subdivided face identity") {
    const auto file = vmf::parse("world { \"id\" \"1\" \"classname\" \"worldspawn\" }");
    REQUIRE(file);
    document::Document doc(*file);
    const auto brush = [](double bottom, double top, std::string material) {
        const std::array<geometry::PlaneSide, 6> planes{{
            {{{}, "", {}, 16, 0}, {{1, 0, 0}, 4096}}, {{{}, "", {}, 16, 0}, {{-1, 0, 0}, 0}},
            {{{}, "", {}, 16, 0}, {{0, 1, 0}, 128}}, {{{}, "", {}, 16, 0}, {{0, -1, 0}, 0}},
            {{{}, "", {}, 16, 0}, {{0, 0, 1}, top}}, {{{}, "", {}, 16, 0}, {{0, 0, -1}, -bottom}}}};
        auto poly = geometry::ConvexPolyhedron::fromPlanes(planes);
        REQUIRE(poly);
        return geometry::solidBlock(*poly, {std::move(material)});
    };
    auto floor = brush(-16, 0, "brick/test");
    auto sides = floor.findAll("side");
    sides[0]->find("material")->setValue("tools/toolsnodraw").value();
    sides[2]->find("material")->setValue("tools/toolsclip").value();
    sides[5]->find("material")->setValue("custom/nolight").value();
    auto tx = doc.begin("two brushes");
    (void)tx.create(std::move(floor), doc.handle(doc.roots()[0]));
    auto detail = kv1::Node::block("entity");
    detail.append(kv1::Node::value("classname", "func_detail")).value();
    detail.append(brush(128, 144, "tools/toolsskybox")).value();
    (void)tx.create(std::move(detail));
    tx.commit();
    const auto material = [](std::string_view name) {
        BakeMaterial result;
        result.noLight = name == "custom/nolight";
        return result;
    };
    const auto scene = bakeSceneFromDocument(doc, {}, material);
    REQUIRE(scene);
    const auto casters = bakeCastersFromDocument(doc, scene->faces, material);
    REQUIRE(casters);
    const auto hitAt = [&](Vec3 origin, Vec3 direction, float reach = 1000) {
        const auto hit = casters->accel->closest(origin, direction, reach);
        REQUIRE(hit);
        return *hit;
    };
    const auto left = hitAt({100, 64, 64}, {0, 0, -1});
    const auto right = hitAt({3000, 64, 64}, {0, 0, -1});
    const auto a = casters->faceOf(left), b = casters->faceOf(right);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a != *b);
    CHECK(scene->faces[*a].solidId == scene->faces[*b].solidId);
    CHECK(scene->faces[*a].sideId == scene->faces[*b].sideId);
    CHECK(left.distance == doctest::Approx(64));
    CHECK_FALSE(casters->isSky(left));
    const auto sky = hitAt({100, 64, 64}, {0, 0, 1});
    CHECK(casters->isSky(sky));
    CHECK_FALSE(casters->faceOf(sky));
    CHECK(casters->reachesSky({100, 64, 64}, {0, 0, 1}));
    const auto nodraw = hitAt({4200, 64, -8}, {-1, 0, 0}, 110);
    CHECK_FALSE(casters->faceOf(nodraw));
    CHECK_FALSE(casters->isSky(nodraw));
    CHECK_FALSE(casters->visible({4200, 64, -8}, {4090, 64, -8}));
    const auto noLight = hitAt({100, 64, -32}, {0, 0, 1});
    CHECK_FALSE(casters->faceOf(noLight));
    CHECK_FALSE(casters->isSky(noLight));
    CHECK_FALSE(casters->accel->occluded({100, 256, -8}, {0, -1, 0}, 130)); // clip
    const auto empty = bakeCastersFromDocument(document::Document(*file), {});
    REQUIRE(empty);
    CHECK_FALSE(empty->accel->closest({0, 0, 1}, {0, 0, -1}, 100));
}
