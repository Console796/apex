#include "../assets/fixture.hpp"
#include "rayquery_probe.hpp"
#include "shared_context.hpp"
#include "render/render_mirror.hpp"
#include "render/scene_accel.hpp"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <fstream>
#include <random>
#include <set>
#include <tuple>

using namespace apex;
using namespace apex::render;

namespace {

void checkSet(const SceneAccel& scene, const std::vector<MeshDraw>& casters) {
    std::size_t instances = 0, triangles = 0;
    std::set<std::tuple<VkBuffer, std::uint32_t, std::uint32_t>> meshes;
    for (const auto& draw : casters) {
        instances += draw.instanceCount;
        triangles += std::size_t(draw.indexCount / 3) * draw.instanceCount;
        if (draw.instanceCount) meshes.emplace(draw.vertices->handle(), draw.firstIndex, draw.indexCount);
    }
    CHECK(scene.instanceCount() == instances);
    CHECK(scene.triangleCount() == triangles);
    CHECK(scene.blasCount() == meshes.size());
}

void crossCheck(Context& context, const SceneAccel& scene, std::span<const testing::Ray> rays) {
    std::vector<std::optional<lighting::RayHit>> cpuHits;
    std::size_t hits = 0;
    for (const auto& ray : rays) {
        const Vec3 origin(ray.origin[0], ray.origin[1], ray.origin[2]);
        const Vec3 direction(ray.direction[0], ray.direction[1], ray.direction[2]);
        auto hit = scene.closest(origin, direction, ray.direction[3]);
        CHECK(scene.occluded(origin, direction, ray.direction[3]) == hit.has_value());
        hits += hit.has_value();
        cpuHits.push_back(hit);
    }
    CHECK(hits > 0);
    CHECK(hits < rays.size());
    if (!context.info().rayQuery) { MESSAGE("Skipped GPU half: ray query unavailable; CPU queries passed"); return; }
    REQUIRE(scene.tlas() != VK_NULL_HANDLE);
    const auto gpuHits = testing::traceRays(&context, scene.tlas(), rays);
    for (std::size_t i = 0; i < rays.size(); ++i) {
        CAPTURE(i);
        CHECK(bool(gpuHits[i].hit) == cpuHits[i].has_value());
        if (!gpuHits[i].hit || !cpuHits[i]) continue;
        CHECK(gpuHits[i].distance == doctest::Approx(cpuHits[i]->distance).epsilon(2e-4).scale(1));
        CHECK(gpuHits[i].customIndex == cpuHits[i]->customIndex);
        CHECK(gpuHits[i].primitive == cpuHits[i]->primitive);
    }
}

} // namespace

TEST_CASE("render: scene acceleration matches real shadow caster list and ray queries") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    MESSAGE("Scene ray-query device: " << context->info().name << "; validation=" << context->info().validation);
    context->validation().clear();
    {
        GameTree tree;
        tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
        JobPool jobs(2);
        assets::GameProfile profile; profile.gameDirectory = tree.root;
        auto vfs = assets::Vfs::load(profile, jobs); REQUIRE(vfs);
        assets::MaterialCache materials(*vfs, jobs);
        auto heap = TextureHeap::create(*context); REQUIRE(heap);
        std::ifstream stream(std::filesystem::path(APEX_FIXTURE_DIR) / "vmf/render_map.vmf"); REQUIRE(stream.good());
        const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
        auto file = vmf::parse(bytes); REQUIRE(file);
        document::Document doc(*file);
        RenderMirror mirror(*context, **heap, jobs, materials);
        mirror.rebuild(doc); REQUIRE(mirror.settle());
        MirrorViewOptions options; options.mode = DisplayMode::Lit;
        auto list = mirror.buildDrawList(options);
        REQUIRE(list.shadowCasters);
        REQUIRE_FALSE(list.shadowCasters->empty());
        SceneAccel cpu;
        SceneAccel scene(context);
        REQUIRE(cpu.sync(*list.shadowCasters, list.previews) == SceneAccelChange::Rebuilt);
        REQUIRE(scene.sync(*list.shadowCasters, list.previews) == SceneAccelChange::Rebuilt);
        checkSet(cpu, *list.shadowCasters);
        checkSet(scene, *list.shadowCasters);
        CHECK(cpu.tlas() == VK_NULL_HANDLE);
        CHECK(scene.sync(*list.shadowCasters, list.previews) == SceneAccelChange::Unchanged);

        std::mt19937 random(0x073);
        std::uniform_real_distribution<float> xy(-400, 400), z(-100, 400);
        std::vector<testing::Ray> rays;
        for (int i = 0; i < 2048; ++i) {
            const Vec3 origin(xy(random), xy(random), z(random));
            const Vec3 target(xy(random), xy(random), z(random));
            const auto direction = glm::normalize(target - origin);
            rays.push_back({{origin.x, origin.y, origin.z, 0}, {direction.x, direction.y, direction.z, 2000}});
        }
        crossCheck(*context, scene, rays);
        // A brush drag changes only one expanded instance; no remesh or new BLAS.
        auto& draw = list.shadowCasters->front();
        list.previews.push_back({glm::translate(DMat4(1), DVec3(37, 13, 9)), draw.origin});
        draw.previewIndex = static_cast<std::uint32_t>(list.previews.size());
        const auto handle = scene.tlas();
        const auto blasCount = scene.blasCount();
        REQUIRE(cpu.sync(*list.shadowCasters, list.previews) == SceneAccelChange::Updated);
        REQUIRE(scene.sync(*list.shadowCasters, list.previews) == SceneAccelChange::Updated);
        CHECK(scene.tlas() == handle);
        CHECK(scene.blasCount() == blasCount);
        checkSet(scene, *list.shadowCasters);
        crossCheck(*context, scene, rays);
        REQUIRE(scene.rebuild(*list.shadowCasters, list.previews));
        crossCheck(*context, scene, rays);
        REQUIRE(scene.sync({}, {}) == SceneAccelChange::Rebuilt);
        CHECK(scene.instanceCount() == 0);
        CHECK(scene.triangleCount() == 0);
        CHECK(scene.blasCount() == 0);
        CHECK(scene.tlas() == VK_NULL_HANDLE);
        CHECK_FALSE(scene.closest({0, 0, 100}, {0, 0, -1}, 1000));
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
    CHECK(context->validation().warningCount() == 0);
}

TEST_CASE("render: scene acceleration shares BLAS ranges and composes instance previews") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    auto vb = Buffer::create(*context, {4 * sizeof(BrushVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT}); REQUIRE(vb);
    auto ib = Buffer::create(*context, {6 * sizeof(std::uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT}); REQUIRE(ib);
    auto mesh = std::make_shared<lighting::TriangleMesh>();
    mesh->positions = {{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}}};
    mesh->indices = {0, 1, 2, 0, 2, 3};
    MeshDraw draw;
    draw.vertices = &*vb; draw.indices = &*ib; draw.cpuMesh = mesh;
    draw.indexCount = 6; draw.origin = DVec3(100, 200, 300);
    draw.previewIndex = 1; draw.instanceCount = 2; draw.instancedTransforms = true;
    std::vector<MeshDraw> casters{draw};
    std::vector<DrawPreview> previews{{glm::translate(DMat4(1), DVec3(-100, -200, -300)), draw.origin},
        {glm::translate(DMat4(1), DVec3(4, 0, 2)) * glm::scale(DMat4(1), DVec3(-1, 2, 1)), DVec3(0)}};
    // First preview maps the nonzero draw origin to zero; the second has its own origin.
    SceneAccel scene(context);
    REQUIRE(scene.sync(casters, previews) == SceneAccelChange::Rebuilt);
    checkSet(scene, casters);
    CHECK(scene.blasCount() == 1);
    const auto hit = scene.closest({0.25f, 0, 10}, {0, 0, -1}, 100); REQUIRE(hit);
    CHECK(hit->distance == doctest::Approx(10)); CHECK(hit->customIndex == 0);
    auto moved = scene.closest({4.25f, 0, 10}, {0, 0, -1}, 100); REQUIRE(moved);
    CHECK(moved->distance == doctest::Approx(8)); CHECK(moved->customIndex == 1);
    const auto handle = scene.tlas();
    previews[1].matrix[3][0] = 8; previews[1].matrix[3][2] = 4;
    REQUIRE(scene.sync(casters, previews) == SceneAccelChange::Updated);
    CHECK(scene.tlas() == handle);
    CHECK_FALSE(scene.closest({4.25f, 0, 10}, {0, 0, -1}, 100));
    moved = scene.closest({8.25f, 0, 10}, {0, 0, -1}, 100); REQUIRE(moved);
    CHECK(moved->distance == doctest::Approx(6)); CHECK(moved->customIndex == 1);
    const std::array<testing::Ray, 4> rays{{{{0.25f, 0, 10, 0}}, {{4.25f, 0, 10, 0}},
                                          {{8.25f, 0, 10, 0}}, {{50, 0, 10, 0}}}};
    crossCheck(*context, scene, rays);
    casters[0].firstIndex = 3; casters[0].indexCount = 3;
    casters.push_back(draw);
    REQUIRE(scene.sync(casters, previews) == SceneAccelChange::Rebuilt);
    checkSet(scene, casters);
    CHECK(scene.blasCount() == 2);
    casters[0].previewIndex = 999;
    CHECK_FALSE(scene.sync(casters, previews));
    CHECK(scene.blasCount() == 2); // Rejected input leaves the built scene intact.
}
