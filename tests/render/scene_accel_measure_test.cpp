#include "../editor/corpus.hpp"
#include "editor/instance_expansion.hpp"
#include "render/render_mirror.hpp"
#include "render/scene_accel.hpp"
#include "shared_context.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <iostream>

using namespace apex;
using namespace apex::render;

TEST_CASE("scene acceleration: measure largest expanded corpus map brush drag") {
    const char* enabled = std::getenv("APEX_MEASURE");
    if (!enabled || std::string_view(enabled) != "1") { MESSAGE("APEX_MEASURE=1 is required"); return; }
    const auto paths = editor_test::corpusPaths();
    if (paths.empty()) { MESSAGE("Skipped: APEX_P2CE_DIR unset"); return; }
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    assets::GameProfile profile;
    profile.gameDirectory = std::filesystem::path(std::getenv("APEX_P2CE_DIR")) / "p2ce";
    editor::InstanceTemplates templates;
    const auto solidCount = [](const document::Document& doc) {
        std::size_t count = 0;
        for (auto node : editor_test::nodes(doc)) count += std::holds_alternative<Handle<document::Solid>>(node);
        return count;
    };
    std::filesystem::path largest;
    std::size_t largestSolids = 0;
    for (const auto& path : paths) {
        document::Document doc(editor_test::readCorpus(path));
        editor::InstanceExpansion expansion(templates);
        expansion.update(doc, path, profile.gameDirectory / "maps");
        auto count = solidCount(doc);
        for (const auto& content : expansion.contents()) count += solidCount(*content.keys);
        if (count > largestSolids) { largest = path; largestSolids = count; }
    }
    REQUIRE_FALSE(largest.empty());
    JobPool jobs(3);
    auto vfs = assets::Vfs::load(profile, jobs); REQUIRE(vfs);
    assets::MaterialCache materials(*vfs, jobs);
    assets::ModelCache models(*vfs, jobs);
    auto heap = TextureHeap::create(*context); REQUIRE(heap);
    document::Document doc(editor_test::readCorpus(largest));
    editor::InstanceExpansion expansion(templates);
    expansion.update(doc, largest, profile.gameDirectory / "maps");
    RenderMirror mirror(*context, **heap, jobs, materials, nullptr, &models,
                        [&](const std::string& material) { return vfs->exists(material); });
    mirror.rebuild(doc);
    mirror.setInstanceContents(expansion.contents());
    REQUIRE(mirror.settle(std::chrono::seconds(120)));
    MirrorViewOptions options; options.mode = DisplayMode::Lit;
    const auto list = mirror.buildDrawList(options);
    REQUIRE(list.shadowCasters);
    REQUIRE_FALSE(list.shadowCasters->empty());
    std::cout << "[scene corpus] maps=" << paths.size() << " largest=" << largest.string()
              << " expanded_source_solids=" << largestSolids << " unresolved_instances=" << expansion.problems().size()
              << " mesh_failures=" << mirror.stats().meshFailures << " device=" << context->info().name
              << " validation=" << context->info().validation << '\n';
    using Clock = std::chrono::steady_clock;
    const auto measure = [&](Context* gpu) {
        auto casters = *list.shadowCasters;
        auto previews = list.previews;
        SceneAccel scene(gpu);
        REQUIRE(scene.rebuild(casters, previews)); // BLAS construction excluded from samples.
        std::vector<double> builds, updates;
        for (int sample = 0; sample < 8; ++sample) {
            const auto buildStart = Clock::now();
            REQUIRE(scene.rebuild(casters, previews));
            const auto built = Clock::now();
            auto& draw = casters.front();
            if (draw.previewIndex) previews[draw.previewIndex - 1].matrix[3][0] += 1;
            else draw.origin.x += 1;
            const auto handle = scene.tlas();
            const auto updateStart = Clock::now();
            REQUIRE(scene.sync(casters, previews) == SceneAccelChange::Updated);
            const auto updated = Clock::now();
            CHECK(scene.tlas() == handle);
            if (sample == 0) continue;
            builds.push_back(std::chrono::duration<double, std::milli>(built - buildStart).count());
            updates.push_back(std::chrono::duration<double, std::milli>(updated - updateStart).count());
        }
        std::ranges::sort(builds); std::ranges::sort(updates);
        std::cout << "[scene timing] backend=" << (gpu && gpu->info().rayQuery ? "Embree+Vulkan" : "Embree")
                  << " instances=" << scene.instanceCount() << " triangles=" << scene.triangleCount()
                  << " unique_blas=" << scene.blasCount() << " rebuild_ms=" << builds[3]
                  << " (" << builds.front() << '-' << builds.back() << ") update_ms=" << updates[3]
                  << " (" << updates.front() << '-' << updates.back() << ")\n";
    };
    measure(nullptr);
    if (context->info().rayQuery) measure(context);
    else MESSAGE("Skipped Vulkan timing: ray query unavailable");
    CHECK(context->validation().errorCount() == 0);
}
