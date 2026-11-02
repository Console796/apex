#include "editor/bake_instances.hpp"
#include "geometry/solid_block.hpp"
#include "lighting/bake_session.hpp"
#include "render/bake_setup.hpp"
#include "render/lightmap_atlas.hpp"
#include "render/solid_mesh.hpp"
#include "../geometry/fixtures.hpp"

#include <doctest/doctest.h>
#include <chrono>
#include <fstream>
#include <set>

using namespace apex;
using namespace apex::render;

namespace {
// A temporary map directory holding one small template (a 64x32x16 box) and a parent map that
// places it twice: once rotated 90 degrees and offset, once unrotated, under one point light.
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("apex-instlm-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    document::Document map{[] {
        auto parsed = vmf::parse(R"(world { id 1 classname worldspawn }
            entity { id 2 classname func_instance file box.vmf origin "100 0 0" angles "0 90 0" }
            entity { id 3 classname func_instance file box.vmf origin "-200 50 0" angles "0 0 0" }
            entity { id 4 classname light origin "0 0 200" _light "255 255 255 20000" })");
        REQUIRE(parsed);
        return std::move(*parsed);
    }()};
    document::Document templ{[] {
        auto parsed = vmf::parse(R"(world { id 1 classname worldspawn })");
        REQUIRE(parsed);
        return std::move(*parsed);
    }()};
    std::vector<document::InstanceSolidOrigin> origins;
    BakeSetup setup;
    LightmapAtlas atlas;

    Fixture() {
        auto tx = templ.begin("box");
        tx.create(geometry::solidBlock(geometry_test::build(geometry_test::boxSides({0, 0, 0}, {64, 32, 16})), {"brick/test"}),
                  templ.handle(templ.roots()[0]));
        tx.commit();
        std::filesystem::create_directories(root);
        std::ofstream(root / "box.vmf") << templ.write().value();
        origins = editor::expandBakeInstances(map, root / "map.vmf");
        setup = setupBake(map, nullptr);
        auto packed = LightmapAtlas::pack(setup.scene.faces);
        REQUIRE(packed);
        atlas = std::move(*packed);
        atlas.bindInstances(origins);
    }
    ~Fixture() { std::filesystem::remove_all(root); }

    // The template's one solid, as the viewport's child mirror would mesh it.
    const kv1::Node& templateSolid() const {
        for (const auto id : templ.roots()) for (const auto child : templ.get(*templ.handle(id))->children)
            if (std::holds_alternative<Handle<document::Solid>>(*templ.handle(child)))
                return *templ.get(*templ.handle(child))->block;
        FAIL("template has no solid");
        std::abort();
    }
    LightmapPlacement placement(const document::InstancePlacement& which) const {
        LightmapPlacement result;
        for (const auto& [solid, origin] : atlas.instances.at(which)) result[solid] = origin;
        return result;
    }
    Result<SolidMesh> mesh(const document::InstancePlacement& which) const {
        const auto layout = placement(which);
        return meshSolid(templateSolid(), [](std::string_view) { return TextureSize{}; }, &atlas, 0, &layout);
    }
    document::InstancePlacement placementOf(document::NodeId owner) const { return {owner, {}}; }
    std::vector<document::NodeId> owners() const {
        std::set<document::NodeId> unique;
        for (const auto& origin : origins) unique.insert(origin.placement.owner);
        return {unique.begin(), unique.end()};
    }
};

// Rectangles (atlas uv space) of every fragment of the expanded face a template side maps to.
std::vector<std::pair<DVec2, DVec2>> rects(const Fixture& f, const document::InstanceSolidOrigin& origin, int side) {
    std::vector<std::pair<DVec2, DVec2>> result;
    for (const auto fragment : f.atlas.fragments.at({origin.solid, origin.sides.at(side)})) {
        const auto& r = f.atlas.rects[fragment];
        result.push_back({DVec2(r.x, r.y) / double(f.atlas.size), DVec2(r.x + r.width, r.y + r.height) / double(f.atlas.size)});
    }
    return result;
}
} // namespace

TEST_CASE("a rotated instance's faces map to the atlas slots baked for their expanded faces") {
    Fixture f;
    REQUIRE(f.origins.size() == 2);
    const auto& rotated = f.origins[0].transform != DMat4(1.0) && f.origins[0].transform[0][0] < 0.5 ? f.origins[0] : f.origins[1];
    CHECK(std::abs(rotated.transform[0][0]) < 1e-9); // 90 degree yaw
    CHECK(rotated.transform[3] == DVec4(100, 0, 0, 1));
    REQUIRE(f.atlas.instances.contains(rotated.placement));
    const auto mesh = f.mesh(rotated.placement);
    REQUIRE(mesh);
    std::size_t mapped = 0;
    for (const auto& batch : mesh->batches) for (const auto& face : batch.faces) {
        const auto slots = rects(f, rotated, int(face.side));
        // Every vertex of the face is drawn inside one of the fragment rectangles (within the guard texel).
        for (std::uint32_t i = face.firstIndex; i < face.firstIndex + face.indexCount; ++i) {
            const auto& v = batch.vertices[batch.indices[i]];
            const DVec2 uv{v.lightmapUv[0], v.lightmapUv[1]};
            bool inside = false;
            for (const auto& [lo, hi] : slots) {
                const double guard = 0.5 / f.atlas.size;
                inside = inside || (uv.x >= lo.x - guard && uv.y >= lo.y - guard && uv.x <= hi.x + guard && uv.y <= hi.y + guard);
            }
            CHECK(inside);
        }
        // Drawn template-local positions, moved by the placement transform, lie on the baked world loop.
        const auto& baked = f.atlas.faces[f.atlas.fragments.at({rotated.solid, rotated.sides.at(int(face.side))}).front()];
        const auto& v = batch.vertices[batch.indices[face.firstIndex]];
        const DVec3 local = mesh->origin + DVec3(v.position[0], v.position[1], v.position[2]);
        const DVec3 world(rotated.transform * DVec4(local, 1.0));
        CHECK(std::abs(glm::dot(baked.normal, world - baked.vertices[0])) < 0.05);
        ++mapped;
    }
    CHECK(mapped == 6);
}

TEST_CASE("two placements of one instance file get distinct lightmaps") {
    Fixture f;
    const auto owners = f.owners();
    REQUIRE(owners.size() == 2);
    CHECK(f.atlas.instances.size() == 2);
    const auto a = f.mesh(f.placementOf(owners[0]));
    const auto b = f.mesh(f.placementOf(owners[1]));
    REQUIRE(a);
    REQUIRE(b);
    const auto slots = [&](const document::NodeId owner) {
        std::set<std::size_t> fragments;
        for (const auto& [solid, origin] : f.atlas.instances.at(f.placementOf(owner)))
            for (const auto& [side, expanded] : origin.sides)
                for (const auto fragment : f.atlas.fragments.at({origin.solid, expanded})) fragments.insert(fragment);
        return fragments;
    };
    const auto first = slots(owners[0]), second = slots(owners[1]);
    CHECK(first.size() >= 6);
    CHECK(second.size() >= 6);
    for (const auto fragment : first) CHECK_FALSE(second.contains(fragment));
    // The same template mesh is laid out differently: the lightmap UVs differ between placements.
    std::vector<float> uvA, uvB;
    for (const auto& batch : a->batches) for (const auto& v : batch.vertices) uvA.insert(uvA.end(), v.lightmapUv, v.lightmapUv + 2);
    for (const auto& batch : b->batches) for (const auto& v : batch.vertices) uvB.insert(uvB.end(), v.lightmapUv, v.lightmapUv + 2);
    CHECK(uvA != uvB);
}

TEST_CASE("a live bake session over instances publishes lit instance faces") {
    Fixture f;
    lighting::BakeSessionOptions options;
    options.baker.threads = 1;
    options.baker.skyRays = options.baker.bounceRays = 4;
    options.initialPasses = 1;
    options.maxPasses = 1;
    lighting::BakeSession session(f.setup.scene, options);
    std::set<std::size_t> lit;
    for (int i = 0; i < 50 && !session.progress().idle; ++i)
        for (const auto& update : session.tick(CancelToken{}, std::chrono::milliseconds(40)).faces)
            for (const auto& value : update.radiance)
                if (value.x + value.y + value.z > 0.0f) { lit.insert(update.face); break; }
    INFO("lights=" << f.setup.scene.lights.size() << " faces=" << f.setup.scene.faces.size() << " lit=" << lit.size() << " passes=" << session.progress().passes.size());
    std::set<document::InstancePlacement> seen;
    for (const auto face : lit) {
        const auto& source = f.setup.scene.faces[face];
        if (const auto found = f.atlas.templateOfSide.find({source.solidId, source.sideId}); found != f.atlas.templateOfSide.end())
            seen.insert(found->second.placement);
    }
    CHECK(seen.size() == 2); // Both placements have published, lit faces.
}
