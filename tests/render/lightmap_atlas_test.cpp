#include "render/lightmap_atlas.hpp"
#include "render/vrad_lightmaps.hpp"
#include "render/solid_mesh.hpp"
#include "render/bake_setup.hpp"
#include "lighting/baker.hpp"
#include "document/vmf.hpp"
#include "geometry/solid_block.hpp"
#include "../assets/fixture.hpp"
#include "../assets/texture_fixture.hpp"
#include "../geometry/fixtures.hpp"

#include <doctest/doctest.h>
#include <map>
#include <set>

using namespace apex;
using namespace apex::render;

TEST_CASE("bake setup: patched material reflectivity overrides VTF without changing direct light") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    const auto bytes = whiteVtf().bake();
    tree.put("materials/test/base.vtf", {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    tree.put("materials/test/base.vmt", "LightmappedGeneric { $basetexture test/base }");
    DVec3 expected{0.7, 0.6, 0.8};
    std::string value = "[0.7 0.6 0.8]";
    SUBCASE("patch override") {}
    SUBCASE("black override") { value = "[0 0 0]"; expected = DVec3{0}; }
    SUBCASE("uniform override") { value = "0.3"; expected = DVec3{0.3}; }
    SUBCASE("out of range clamps") { value = "[-1 2 0.5]"; expected = {0, 1, 0.5}; }
    SUBCASE("malformed falls back to VTF") { value = "bad"; expected = {0.125, 0.25, 0.5}; }
    SUBCASE("absent falls back to VTF") { value.clear(); expected = {0.125, 0.25, 0.5}; }
    tree.put("materials/test/patched.vmt", "patch { include test/base insert { " +
        (value.empty() ? std::string{} : "$reflectivity \"" + value + "\"") + " } }");
    auto file = vmf::parse("world { \"id\" \"1\" \"classname\" \"worldspawn\" }");
    REQUIRE(file);
    document::Document doc(*file);
    const auto poly = geometry_test::build(geometry_test::boxSides());
    auto tx = doc.begin("fixture");
    tx.create(geometry::solidBlock(poly, {"test/patched"}), doc.handle(doc.roots()[0]));
    tx.commit();
    JobPool jobs(1);
    assets::GameProfile profile;
    profile.gameDirectory = tree.root;
    auto vfs = assets::Vfs::load(profile, jobs);
    REQUIRE(vfs);
    auto setup = setupBake(doc, &*vfs);
    REQUIRE(setup.failedMaterials == 0);
    REQUIRE(setup.failedTextures == 0);
    REQUIRE_FALSE(setup.scene.faces.empty());
    for (const auto& face : setup.scene.faces)
        for (int c = 0; c < 3; ++c) CHECK(face.albedo[c] == doctest::Approx(expected[c]));
    auto light = lighting::Light{};
    light.origin = {0, 0, 128}; light.constant = 1; light.intensity = DVec3{1};
    setup.scene.lights = {light};
    lighting::Baker overridden(setup.scene);
    for (auto& face : setup.scene.faces) face.albedo = Vec3{0.125f, 0.25f, 0.5f};
    lighting::Baker textureOnly(setup.scene);
    overridden.step(); textureOnly.step();
    CHECK(overridden.resolveLevel(0) == textureOnly.resolveLevel(0));
}

TEST_CASE("lightmap atlas: shelves are deterministic, guarded and nonoverlapping") {
    std::vector<lighting::BakeFace> faces(80);
    for (std::size_t i = 0; i < faces.size(); ++i) {
        faces[i].chart.dimensions = {1 + int(i % 13), 1 + int(i % 17)};
        faces[i].solidId = i / 2;
        faces[i].sideId = 7;
    }
    auto a = LightmapAtlas::pack(faces, 256);
    auto b = LightmapAtlas::pack(faces, 256);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->size == b->size);
    CHECK(a->rects == b->rects);
    CHECK(a->fragments.at({0, 7}) == std::vector<std::size_t>{0, 1});
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto r = a->rects[i];
        CHECK(r.x >= 1);
        CHECK(r.y >= 1);
        CHECK(r.x + r.width < a->size);
        CHECK(r.y + r.height < a->size);
        for (std::size_t j = 0; j < i; ++j) {
            const auto q = a->rects[j];
            CHECK((r.x + r.width + 1 <= q.x - 1 || q.x + q.width + 1 <= r.x - 1 ||
                   r.y + r.height + 1 <= q.y - 1 || q.y + q.height + 1 <= r.y - 1));
        }
    }
    CHECK_FALSE(LightmapAtlas::pack(faces, 16));
    faces[0].chart.dimensions = {0, 1};
    CHECK_FALSE(LightmapAtlas::pack(faces));
    faces[0].chart.dimensions = {4095, 1};
    CHECK_FALSE(LightmapAtlas::pack(faces));
    CHECK_FALSE(LightmapAtlas::pack({}, 0));
    CHECK(LightmapAtlas::pack({})->size == 1);
    faces.resize(1);
    faces[0].chart.dimensions = {14, 14};
    CHECK(LightmapAtlas::pack(faces, 16)->size == 16);
    CHECK_FALSE(LightmapAtlas::pack(faces, 15));
}

TEST_CASE("lightmap atlas: chart sample centres map to texel centres") {
    std::vector<DVec3> vertices{{-1, -3, 8}, {63, -3, 72}, {63, 61, 72}, {-1, 61, 8}};
    geometry::Face face;
    face.vertices = {0, 1, 2, 3};
    face.plane = {glm::normalize(DVec3{-1, 0, 1}), 9 / std::sqrt(2.0)};
    face.uv.uaxis = {1, 1, 0};
    face.uv.vaxis = {0, -1, 0};
    face.uv.shiftU = 8;
    face.uv.scaleU = -0.5;
    auto faces = lighting::bakeFaces(face, vertices);
    auto atlas = LightmapAtlas::pack(faces);
    REQUIRE(atlas);
    const auto& r = atlas->rects[0];
    for (int t = 0; t < r.height; ++t) for (int s = 0; s < r.width; ++s) {
        const auto uv = atlas->uv(0, faces[0].chart.worldAt({s, t}));
        CHECK(uv.x * atlas->size == doctest::Approx(r.x + s + 0.5));
        CHECK(uv.y * atlas->size == doctest::Approx(r.y + t + 0.5));
    }
}

TEST_CASE("lightmap atlas: meshing preserves source identity across chart fragments") {
    auto planes = geometry_test::boxSides({-32, -32, -32}, {4096, 32, 32});
    for (auto& side : planes) {
        side.material = "test/one";
        side.lightmapScale = 16;
        side.uv.uaxis = std::abs(side.plane.normal.x) > 0.5 ? DVec3{0, 1, 0} : DVec3{1, 0, 0};
        side.uv.vaxis = std::abs(side.plane.normal.z) > 0.5 ? DVec3{0, 1, 0} : DVec3{0, 0, 1};
    }
    const auto poly = geometry_test::build(planes);
    std::vector<lighting::BakeFace> faces;
    for (const auto& face : poly.faces) {
        auto fragments = lighting::bakeFaces(face, poly.vertices);
        for (auto& fragment : fragments) { fragment.solidId = 42; faces.push_back(std::move(fragment)); }
    }
    REQUIRE(faces.size() > poly.faces.size());
    auto atlas = LightmapAtlas::pack(faces);
    REQUIRE(atlas);
    const auto size = [](std::string_view) { return TextureSize{64, 64}; };
    auto mesh = meshSolid(poly, size, &*atlas, 42);
    REQUIRE(mesh);
    REQUIRE(mesh->batches.size() == 1);
    const auto& batch = mesh->batches[0];
    CHECK(batch.faces.size() == poly.faces.size());
    std::size_t vertex = 0, fragment = 0;
    for (const auto& source : poly.faces) {
        const auto& indices = atlas->fragments.at({42, source.sideId});
        for (const auto i : indices) for (const auto point : faces[i].vertices) {
            REQUIRE(vertex < batch.vertices.size());
            const auto& v = batch.vertices[vertex++];
            const auto uv = atlas->uv(i, point);
            CHECK(v.lightmapUv[0] == doctest::Approx(uv.x));
            CHECK(v.lightmapUv[1] == doctest::Approx(uv.y));
            CHECK(v.position[0] + mesh->origin.x == doctest::Approx(point.x));
        }
        vertex += source.vertices.size(); // Original edge perimeter, with no internal fragment edges.
        fragment += indices.size();
    }
    CHECK(fragment == faces.size());
    CHECK(vertex == batch.vertices.size());
    CHECK(batch.edges.size() == 48);
    auto unrelated = meshSolid(poly, size, &*atlas, 43);
    REQUIRE(unrelated);
    CHECK(unrelated->batches[0].vertices.size() == 24);
    for (const auto& v : unrelated->batches[0].vertices) CHECK(v.lightmapUv[0] < 0.0f);
}

TEST_CASE("lightmap atlas: stale layout leaves edited brush geometry intact") {
    auto planes = geometry_test::boxSides();
    auto poly = geometry_test::build(planes);
    std::vector<lighting::BakeFace> faces;
    // Only one side is charted: the rest must remain unmapped.
    faces = lighting::bakeFaces(poly.faces[0], poly.vertices);
    for (auto& face : faces) face.solidId = 42;
    auto atlas = LightmapAtlas::pack(faces);
    REQUIRE(atlas);
    for (auto& vertex : poly.vertices) vertex += DVec3{32, 16, 8};
    for (auto& face : poly.faces) face.plane.dist += glm::dot(face.plane.normal, DVec3{32, 16, 8});
    auto mesh = meshSolid(poly, [](std::string_view) { return TextureSize{}; }, &*atlas, 42);
    REQUIRE(mesh);
    CHECK(mesh->origin == DVec3(32, 16, 8));
    for (const auto& batch : mesh->batches) for (const auto& vertex : batch.vertices)
        CHECK(vertex.lightmapUv[0] < 0.0f);

    SUBCASE("live retention samples old radiance but always uses current geometry") {
        atlas->retainStale = true;
        auto retained = meshSolid(poly, [](std::string_view) { return TextureSize{}; }, &*atlas, 42);
        REQUIRE(retained);
        CHECK(retained->origin == mesh->origin);
        const auto& rect = atlas->rects[0];
        unsigned mapped = 0;
        for (const auto& batch : retained->batches) for (const auto& vertex : batch.vertices) {
            if (vertex.lightmapUv[0] < 0) continue;
            ++mapped;
            CHECK(vertex.lightmapUv[0] >= float((rect.x + 0.5) / atlas->size));
            CHECK(vertex.lightmapUv[0] <= float((rect.x + rect.width - 0.5) / atlas->size));
            CHECK(vertex.lightmapUv[1] >= float((rect.y + 0.5) / atlas->size));
            CHECK(vertex.lightmapUv[1] <= float((rect.y + rect.height - 0.5) / atlas->size));
        }
        CHECK(mapped == poly.faces[0].vertices.size());
        REQUIRE(retained->batches.size() == mesh->batches.size());
        for (std::size_t b = 0; b < mesh->batches.size(); ++b) {
            REQUIRE(retained->batches[b].vertices.size() == mesh->batches[b].vertices.size());
            for (std::size_t v = 0; v < mesh->batches[b].vertices.size(); ++v)
                for (int axis = 0; axis < 3; ++axis)
                    CHECK(retained->batches[b].vertices[v].position[axis] == mesh->batches[b].vertices[v].position[axis]);
        }
    }
}

TEST_CASE("lightmap atlas: displacement sides retain their separate shading flag") {
    auto block = geometry::solidBlock(geometry_test::build(geometry_test::boxSides()), {"test/one"});
    auto sides = block.findAll("side");
    for (std::size_t i = 0; i < sides.size(); ++i)
        sides[i]->find("id")->setValue(std::to_string(i + 1)).value();
    sides[4]->append(kv1::Node::block("dispinfo")).value();
    auto mesh = meshSolid(block, [](std::string_view) { return TextureSize{}; });
    REQUIRE(mesh);
    REQUIRE(mesh->batches[0].faces.size() == 6);
    for (const auto& face : mesh->batches[0].faces) CHECK(face.displacement == (face.side == 5));
}

TEST_CASE("lightmap atlas: restart retains same-side radiance with resized charts") {
    lighting::BakeFace face;
    face.solidId = 42;
    face.sideId = 7;
    face.chart.dimensions = {2, 1};
    face.chart.worldToLuxel = {DVec3{1, 0, 0}, DVec3{0, 1, 0}};
    face.chart.luxelToWorld = face.chart.worldToLuxel;
    auto previous = LightmapAtlas::pack(std::array{face});
    REQUIRE(previous);
    face.chart.dimensions = {3, 1};
    auto added = face;
    added.sideId = 8;
    auto current = LightmapAtlas::pack(std::array{face, added});
    REQUIRE(current);
    const std::vector<std::vector<Vec3>> radiance{{Vec3{1}, Vec3{2}}};
    std::vector<char> covered;
    const auto retained = current->retainRadiance(&*previous, radiance, &covered);
    CHECK(covered == std::vector<char>{1, 0});
    CHECK(retained[0] == std::vector<Vec3>{Vec3{1}, Vec3{2}, Vec3{2}});
    CHECK(retained[1] == std::vector<Vec3>(3, Vec3{0}));
    CHECK(current->retainRadiance(nullptr, {}, &covered) == std::vector<std::vector<Vec3>>(2, std::vector<Vec3>(3, Vec3{0})));
    CHECK(covered == std::vector<char>{0, 0});
    VradLightmaps vrad{*previous, radiance, {}};
    lighting::BakeScene scene;
    scene.faces = current->faces;
    const auto state = hybridSeed(*current, scene, vrad, {});
    CHECK(state.passes[0] == std::vector<unsigned>{4, 0});
    CHECK(state.keys == current->faceKeys());
}

TEST_CASE("lightmap atlas: plain faces' keys equal lighting::faceKeys") {
    auto planes = geometry_test::boxSides({-32, -32, -32}, {512, 32, 32});
    for (auto& side : planes) side.material = "test/one";
    const auto poly = geometry_test::build(planes);
    std::vector<lighting::BakeFace> faces;
    for (const auto& face : poly.faces) for (auto& fragment : lighting::bakeFaces(face, poly.vertices)) {
        fragment.solidId = 7;
        faces.push_back(std::move(fragment));
    }
    auto atlas = LightmapAtlas::pack(faces);
    REQUIRE(atlas);
    CHECK(atlas->faceKeys() == lighting::faceKeys(atlas->faces));
}

namespace {
// worldspawn plus brush entities of the given classes, one 64-unit box each, under one point light.
// A `nolight` material has no VMT here, so the name-based tools/ fallback stands in for NOLIGHT.
struct BrushEntityFixture {
    document::Document doc{[] {
        auto parsed = vmf::parse(R"(world { id 1 classname worldspawn }
            entity { id 2 classname light origin "0 0 300" _light "255 255 255 20000" })");
        REQUIRE(parsed);
        return std::move(*parsed);
    }()};
    std::map<std::string, document::NodeId> solidOf;
    document::NodeId add(const std::string& cls, const std::string& material, double x) {
        auto entity = kv1::Node::block("entity");
        entity.append(kv1::Node::value("classname", cls)).value();
        auto tx = doc.begin("add " + cls);
        const auto owner = tx.create(std::move(entity));
        const auto solid = tx.create(geometry::solidBlock(geometry_test::build(geometry_test::boxSides({x, 0, 0}, {x + 64, 64, 64})),
                                                        {material}), owner);
        tx.commit();
        const auto id = doc.get(solid)->id;
        solidOf[cls] = id;
        return id;
    }
    std::size_t fragmentsOf(const BakeSetup& setup, document::NodeId solid) const {
        return std::ranges::count(setup.scene.faces, std::uint64_t(solid), &lighting::BakeFace::solidId);
    }
};
}

TEST_CASE("bake setup: lightmapped brush entities get faces and atlas slots, tool volumes do not") {
    BrushEntityFixture f;
    f.add("func_detail", "brick/test", 0);
    f.add("func_brush", "brick/test", 128);
    f.add("func_illusionary", "brick/test", 256);
    f.add("func_door", "brick/test", 384);
    f.add("trigger_multiple", "brick/test", 512);
    f.add("func_areaportal", "brick/test", 640);
    f.add("func_wall", "tools/toolsnolight", 768);
    const auto setup = setupBake(f.doc, nullptr);
    auto atlas = LightmapAtlas::pack(setup.scene.faces);
    REQUIRE(atlas);
    for (const auto* cls : {"func_detail", "func_brush", "func_illusionary", "func_door"}) {
        CAPTURE(cls);
        const auto solid = f.solidOf.at(cls);
        CHECK(f.fragmentsOf(setup, solid) >= 6);
        std::set<std::uint32_t> sides;
        for (const auto& face : setup.scene.faces) if (face.solidId == solid) sides.insert(face.sideId);
        CHECK(sides.size() == 6);
        for (const auto side : sides) CHECK(atlas->fragments.contains({solid, side}));
    }
    for (const auto* cls : {"trigger_multiple", "func_areaportal", "func_wall"}) {
        CAPTURE(cls);
        CHECK(f.fragmentsOf(setup, f.solidOf.at(cls)) == 0); // Unmapped, as before.
    }
}

TEST_CASE("bake setup: brush-entity face keys are stable across an unrelated edit") {
    BrushEntityFixture f;
    const auto solid = f.add("func_brush", "brick/test", 0);
    const auto keysOf = [&] {
        const auto setup = setupBake(f.doc, nullptr);
        auto atlas = LightmapAtlas::pack(setup.scene.faces);
        REQUIRE(atlas);
        std::set<lighting::FaceKey> mine;
        const auto keys = atlas->faceKeys();
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (atlas->faces[i].solidId == solid) mine.insert(keys[i]);
        return mine;
    };
    const auto before = keysOf();
    CHECK(before.size() >= 6);
    f.add("func_door", "brick/test", 512); // Unrelated: a new entity and solid.
    CHECK(keysOf() == before);
}

TEST_CASE("bake setup: a brush entity is lit but does not cast") {
    BrushEntityFixture f;
    f.add("func_brush", "brick/test", 0);
    const auto setup = setupBake(f.doc, nullptr);
    REQUIRE_FALSE(setup.scene.faces.empty());
    // Only the entity box is in the map, and it is not a caster: a segment through it is unoccluded.
    CHECK(setup.scene.occlusion.visible({-10, 32, 32}, {74, 32, 32}));
    lighting::Baker baker(setup.scene);
    baker.step();
    CHECK(baker.resolve().size() == setup.scene.faces.size());
}

namespace {
void setupSides(std::vector<geometry::PlaneSide>& planes) {
    for (auto& side : planes) {
        side.material = "test/one";
        side.lightmapScale = 16;
        side.uv.uaxis = std::abs(side.plane.normal.x) > 0.5 ? DVec3{0, 1, 0} : DVec3{1, 0, 0};
        side.uv.vaxis = std::abs(side.plane.normal.z) > 0.5 ? DVec3{0, 1, 0} : DVec3{0, 0, 1};
    }
}
// Scales a loop about its centroid.
void scaleLoop(std::vector<DVec3>& loop, double factor) {
    DVec3 centre{0.0};
    for (const auto& p : loop) centre += p;
    centre /= double(loop.size());
    for (auto& p : loop) p = centre + (p - centre) * factor;
}
std::vector<lighting::BakeFace> boxFaces(const geometry::ConvexPolyhedron& poly, std::uint64_t solid, int = 0) {
    std::vector<lighting::BakeFace> faces;
    for (const auto& face : poly.faces)
        for (auto& fragment : lighting::bakeFaces(face, poly.vertices)) { fragment.solidId = solid; faces.push_back(std::move(fragment)); }
    return faces;
}
}

TEST_CASE("lightmap atlas: a compiled atlas keeps fragments that cover no more than the face") {
    auto planes = geometry_test::boxSides();
    setupSides(planes);
    const auto poly = geometry_test::build(planes);
    auto faces = boxFaces(poly, 7);
    REQUIRE(faces.size() == poly.faces.size());
    auto packed = LightmapAtlas::pack(faces);
    REQUIRE(packed);
    const auto size = [](std::string_view) { return TextureSize{64, 64}; };
    const auto lit = [&](const LightmapAtlas& atlas) {
        auto mesh = meshSolid(poly, size, &atlas, 7);
        REQUIRE(mesh);
        std::size_t count = 0;
        for (const auto& batch : mesh->batches) for (const auto& v : batch.vertices) count += v.lightmapUv[0] >= 0.0f;
        return count;
    };
    // The baker's own charts match in either mode.
    const auto all = lit(*packed);
    CHECK(all > 0);
    auto compiled = *packed;
    compiled.compiled = true;
    CHECK(lit(compiled) == all);
    // A fragment the compiler cut down (touching brushes removed part of the side) is still used...
    scaleLoop(compiled.faces[0].vertices, 0.5);
    CHECK(lit(compiled) == all);
    // ...but the baker's strict check refuses the same fragment, since it no longer matches the chart.
    auto strict = compiled;
    strict.compiled = false;
    CHECK(lit(strict) < all);
    // A fragment spilling past the face is rejected whatever the mode.
    auto spilling = *packed;
    spilling.compiled = true;
    scaleLoop(spilling.faces[0].vertices, 2.0);
    CHECK(lit(spilling) < all);
}

TEST_CASE("lightmap atlas: the density overlay has the radiance layout and colours by units per luxel") {
    auto planes = geometry_test::boxSides({0, 0, 0}, {128, 128, 128});
    setupSides(planes);
    planes[0].lightmapScale = 32; // One coarse face.
    planes[1].lightmapScale = 8;  // One fine face.
    const auto poly = geometry_test::build(planes);
    auto faces = boxFaces(poly, 1);
    auto atlas = LightmapAtlas::pack(faces);
    REQUIRE(atlas);
    const auto density = densityRadiance(*atlas);
    REQUIRE(density.size() == atlas->faces.size());
    int green = 0, red = 0, blue = 0;
    for (std::size_t i = 0; i < density.size(); ++i) {
        const auto& chart = atlas->faces[i].chart;
        REQUIRE(density[i].size() == std::size_t(chart.dimensions[0]) * std::size_t(chart.dimensions[1]));
        REQUIRE(density[i].size() >= 2);
        // Neighbouring luxels alternate between a bright and a dim cell of the same hue.
        const auto a = density[i][0], b = density[i][1];
        CHECK(a != b);
        const auto hue = glm::normalize(a);
        const auto other = glm::normalize(b);
        CHECK(hue.x == doctest::Approx(other.x));
        CHECK(hue.y == doctest::Approx(other.y));
        const auto bright = std::max(glm::length(a), glm::length(b)), dim = std::min(glm::length(a), glm::length(b));
        CHECK(dim == doctest::Approx(bright * 0.4f));
        if (hue.x > hue.y && hue.x > hue.z) ++red;
        else if (hue.z > hue.y) ++blue;
        else ++green;
    }
    CHECK(red >= 1);
    CHECK(blue >= 1);
    CHECK(green >= 1);
    CHECK(densityRadiance(*atlas) == density); // Deterministic.
    CHECK(densityLegend(16.0).find("16") != std::string::npos);
}
