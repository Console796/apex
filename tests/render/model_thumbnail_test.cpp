#include "../assets/fixture.hpp"
#include "golden_image.hpp"
#include "shared_context.hpp"
#include "thumbnail_fixture.hpp"
#include "assets/model_index.hpp"
#include "render/model_thumbnail_scheduler.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <iostream>
#include <numeric>
#include <set>
#include <thread>

using namespace apex;
using namespace apex::render;

namespace {
struct CacheFixture {
    GameTree tree;
    JobPool jobs{2};
    std::unique_ptr<assets::Vfs> vfs;
    std::unique_ptr<assets::ThumbnailCache> cache;
    CacheFixture() {
        tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
        assets::GameProfile profile; profile.gameDirectory = tree.root;
        auto loaded = assets::Vfs::load(profile, jobs); REQUIRE(loaded);
        vfs = std::make_unique<assets::Vfs>(std::move(*loaded));
        cache = std::make_unique<assets::ThumbnailCache>(*vfs, jobs, nullptr);
    }
};
constexpr auto kind = assets::ThumbnailKind::Model;
constexpr int size = ModelThumbnailRenderer::kSize;
}

TEST_CASE("model thumbnail golden, empty geometry and render readback timing") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    auto heap = TextureHeap::create(*context); REQUIRE(heap);
    auto renderer = ModelThumbnailRenderer::create(*context, **heap); REQUIRE(renderer);
    auto model = testing::thumbnailModel();
    auto image = (*renderer)->render(model); REQUIRE(image);
    CHECK(image->width == size);
    CHECK(image->height == size);
    CHECK(image->rgba.size() == 65536);
    CHECK(image->rgba[3] == 0);
    CHECK(std::ranges::any_of(image->rgba, [](auto v) { return v == 255; }));
    golden::check("model_thumbnail", {size, size, image->rgba}, *context);
    const double cold = std::chrono::duration<double, std::milli>((*renderer)->lastDuration()).count();
    std::vector<double> timings;
    for (int i = 0; i < 20; ++i) {
        REQUIRE((*renderer)->render(model));
        timings.push_back(std::chrono::duration<double, std::milli>((*renderer)->lastDuration()).count());
    }
    std::cout << "[model thumbnail] " << context->info().name << ": cold " << cold
              << " ms; 20 warm render+upload+readbacks mean "
              << std::accumulate(timings.begin(), timings.end(), 0.0) / double(timings.size())
              << " ms, min " << *std::ranges::min_element(timings)
              << " ms, max " << *std::ranges::max_element(timings) << " ms; RGBA " << image->rgba.size() << " bytes\n";
    CHECK((*renderer)->maxThumbnailSlotsLive() == 0); // Flat rendering never uploads materials.
    CHECK_FALSE((*renderer)->render({}));
    auto invalid = model;
    invalid.indices[0] = 99999;
    CHECK_FALSE((*renderer)->render(invalid));
    invalid = model;
    invalid.submeshes[0].indexCount = 99999;
    CHECK_FALSE((*renderer)->render(invalid));
    // Exercise the existing uint32 GPU path above the uint16 vertex boundary.
    model.vertices.insert(model.vertices.begin(), 65536, {});
    for (auto& index : model.indices) index += 65536;
    auto wide = (*renderer)->render(model); REQUIRE(wide);
    CHECK(wide->rgba == image->rgba);
}

TEST_CASE("model thumbnail scheduler yields to interaction, prioritises, drops and retries loading") {
    CacheFixture fixture;
    auto model = std::make_shared<assets::ModelData>(testing::thumbnailModel());
    std::map<std::string, assets::ModelState> states;
    std::vector<std::string> rendered;
    std::string current;
    int loads = 0;
    ModelThumbnailScheduler scheduler(*fixture.cache,
        {[&](std::string_view p) { return states[std::string(p)]; },
         [&](std::string_view p) -> std::shared_ptr<const assets::ModelData> {
             current = p;
             if (states[current] == assets::ModelState::Unloaded) {
                 states[current] = assets::ModelState::Loading;
                 ++loads;
                 return {};
             }
             return model;
         },
         [](std::string_view) { return std::optional<Error>{Error{ErrorCode::ParseError, "synthetic failure"}}; }},
        [&](const assets::ModelData&) -> Result<assets::Thumbnail> {
            rendered.push_back(current);
            if (model->vertices.empty())
                return std::unexpected(Error{ErrorCode::InvalidArgument, "Model has no triangles"});
            return assets::Thumbnail{size, size, std::vector<std::uint8_t>(size * size * 4, 255)};
        });
    scheduler.options().tickBudget = {};
    for (const auto* name : {"off1", "off2", "visible1", "visible2", "drop"}) {
        states[assets::modelPath(name)] = assets::ModelState::Ready;
        scheduler.request(name, false);
    }
    scheduler.request("VISIBLE1.MDL");
    scheduler.request("visible2");
    scheduler.request("off1", true);
    scheduler.request("off1", false); // Reprioritisation in both directions.
    scheduler.drop("DROP.mdl");
    CHECK(scheduler.pending() == 4);
    CHECK_FALSE(scheduler.tick(true));
    CHECK(rendered.empty());
    REQUIRE(scheduler.tick(false));
    CHECK(rendered == std::vector<std::string>{"visible1.mdl"});
    REQUIRE(scheduler.tick(false));
    CHECK(rendered.back() == "visible2.mdl");
    REQUIRE(scheduler.tick(false));
    CHECK(rendered.back() == "off2.mdl");
    REQUIRE(scheduler.tick(false));
    CHECK(rendered.back() == "off1.mdl");
    scheduler.request("visible1");
    CHECK_FALSE(scheduler.tick(false));
    CHECK(rendered.size() == 4); // Cached, no render.
    scheduler.request("loading");
    CHECK_FALSE(scheduler.tick(true));
    CHECK(loads == 0);
    CHECK_FALSE(scheduler.tick(false));
    CHECK(loads == 1);
    CHECK_FALSE(scheduler.tick(false));
    CHECK(scheduler.pending() == 1);
    states["loading.mdl"] = assets::ModelState::Ready;
    CHECK(scheduler.tick(false));
    CHECK(rendered.size() == 5);
    scheduler.request("failed");
    states["failed.mdl"] = assets::ModelState::Failed;
    CHECK_FALSE(scheduler.tick(false));
    CHECK(fixture.cache->state(kind, "failed", size) == assets::ThumbnailState::Failed);
    REQUIRE(fixture.cache->error(kind, "failed", size));
    scheduler.request("failed");
    CHECK_FALSE(scheduler.tick(false));
    CHECK(rendered.size() == 5);
    model->vertices.clear();
    states["empty.mdl"] = assets::ModelState::Ready;
    scheduler.request("empty");
    CHECK(scheduler.tick(false));
    CHECK(fixture.cache->state(kind, "empty", size) == assets::ThumbnailState::Failed);
    scheduler.request("empty");
    CHECK_FALSE(scheduler.tick(false));
    CHECK(rendered.size() == 6);
}

TEST_CASE("model thumbnails fast scroll 2204 entries bounds texture slots and shared CPU budget") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    CacheFixture fixture;
    auto heap = TextureHeap::create(*context); REQUIRE(heap);
    auto first = (*heap)->reserveThumbnailSlots(); REQUIRE(first);
    auto again = (*heap)->reserveThumbnailSlots(); REQUIRE(again);
    CHECK(*again == *first);
    auto renderer = ModelThumbnailRenderer::create(*context, **heap); REQUIRE(renderer);
    auto model = std::make_shared<assets::ModelData>(testing::thumbnailModel());
    std::string current, firstRendered;
    int renders = 0;
    std::set<std::string> done;
    const std::array<std::byte, 4> pixel{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    ModelThumbnailScheduler scheduler(*fixture.cache,
        {[](std::string_view) { return assets::ModelState::Ready; },
         [&](std::string_view p) { current = p; return model; },
         [](std::string_view) { return std::optional<Error>{}; }},
        [&](const assets::ModelData& data) -> Result<assets::Thumbnail> {
            if (renders++ == 0) firstRendered = current;
            CHECK(done.insert(current).second);
            // Flat mode needs none; stress the real reserved upload/recycling path as well.
            auto slot = (*renderer)->texture(current, {1, 1}, pixel); REQUIRE(slot);
            CHECK(*slot >= *first);
            CHECK(*slot < *first + kThumbnailTextureSlots);
            return (*renderer)->render(data);
        });
    scheduler.options().tickBudget = {};
    constexpr int count = 2204, window = 31;
    const auto name = [](int i) { return "models/synthetic/" + std::to_string(i); };
    for (int i = 0; i < count; ++i) scheduler.request(name(i), false);
    int previous = 0;
    for (int tick = 0; scheduler.pending() && tick < count * 3; ++tick) {
        if (tick % 3 == 0) {
            const int start = (tick * 73) % count;
            for (int i = 0; i < window; ++i)
                if (!done.contains(assets::modelPath(name((previous + i) % count))))
                    scheduler.request(name((previous + i) % count), false);
            for (int i = 0; i < window; ++i)
                if (!done.contains(assets::modelPath(name((start + i) % count))))
                    scheduler.request(name((start + i) % count), true);
            previous = start;
        }
        const int before = renders;
        const bool interacting = tick % 7 == 0;
        scheduler.tick(interacting);
        REQUIRE(renders - before <= (interacting ? 0 : 1));
        REQUIRE((*heap)->thumbnailSlotsLive() <= 64);
        REQUIRE(fixture.cache->residentBytes() <= 64 * 1024 * 1024);
    }
    REQUIRE(scheduler.pending() == 0);
    REQUIRE(renders == count);
    CHECK(fixture.cache->state(kind, firstRendered, size) == assets::ThumbnailState::Unloaded);
    const int before = renders;
    done.erase(firstRendered);
    scheduler.request(firstRendered);
    CHECK(scheduler.tick(false));
    CHECK(renders == before + 1);
    CHECK(fixture.cache->state(kind, firstRendered, size) == assets::ThumbnailState::Ready);
    CHECK((*renderer)->maxThumbnailSlotsLive() == 64);
    // A recently used slot survives replacement of the least recently used one.
    auto hot = (*renderer)->texture("hot", {1, 1}, pixel); REQUIRE(hot);
    for (int i = 0; i < 64; ++i) {
        auto touched = (*renderer)->texture("hot", {1, 1}, pixel); REQUIRE(touched);
        CHECK(*touched == *hot);
        REQUIRE((*renderer)->texture("replacement/" + std::to_string(i), {1, 1}, pixel));
    }
    auto touched = (*renderer)->texture("hot", {1, 1}, pixel); REQUIRE(touched);
    CHECK(*touched == *hot);
    (*renderer).reset();
    CHECK((*heap)->thumbnailSlotsLive() == 0);
    CHECK((*heap)->maxThumbnailSlotsLive() == 64);
    // Normal uploads cannot use even the now-empty reserved slots.
    std::vector<TextureSlot> mapSlots;
    for (std::uint32_t i = 0; i < (*heap)->capacity() - 65; ++i) {
        auto slot = (*heap)->upload({1, 1}, pixel); REQUIRE(slot);
        REQUIRE((*slot < *first || *slot >= *first + 64));
        mapSlots.push_back(*slot);
    }
    CHECK_FALSE((*heap)->upload({1, 1}, pixel));
    REQUIRE((*heap)->flush());
    for (auto slot : mapSlots) (*heap)->release(slot);
    (*heap)->collectGarbage();
}

TEST_CASE("textured model thumbnail golden, flat fallback and bounded texture slots") {
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    auto heap = TextureHeap::create(*context); REQUIRE(heap);
    auto renderer = ModelThumbnailRenderer::create(*context, **heap); REQUIRE(renderer);
    auto model = testing::thumbnailModel();
    auto flat = (*renderer)->render(model); REQUIRE(flat);
    const ModelMaterials materials{{0, {"apex/thumbnail", testing::thumbnailMaterial()}}};
    auto textured = (*renderer)->render(model, materials); REQUIRE(textured);
    CHECK(textured->rgba != flat->rgba);
    // The checker's two colours both reach the image; the flat tint (166, 184, 204) is gone.
    const auto count = [&](auto predicate) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < textured->rgba.size(); i += 4)
            n += textured->rgba[i + 3] == 255 && predicate(textured->rgba[i], textured->rgba[i + 1], textured->rgba[i + 2]);
        return n;
    };
    CHECK(count([](int r, int, int b) { return r > b + 60; }) > 500);
    CHECK(count([](int r, int, int b) { return b > r + 60; }) > 500);
    golden::check("model_thumbnail_textured", {size, size, textured->rgba}, *context);
    CHECK((*renderer)->maxThumbnailSlotsLive() == 2); // The material and the white fill.
    // Cached by name: another render of the same material uploads nothing new.
    auto again = (*renderer)->render(model, materials); REQUIRE(again);
    CHECK(again->rgba == textured->rgba);
    CHECK((*renderer)->maxThumbnailSlotsLive() == 2);
    // A material with no usable texture draws the flat tint.
    auto bare = std::make_shared<assets::CachedMaterial>();
    auto fallback = (*renderer)->render(model, {{0, {"apex/bare", bare}}}); REQUIRE(fallback);
    CHECK(fallback->rgba == flat->rgba);
    // Very large textures use a smaller mip, so a 1024 texel texture still renders.
    auto large = (*renderer)->render(model, {{0, {"apex/large", testing::thumbnailMaterial({230, 60, 40}, {40, 90, 230}, 1024)}}});
    REQUIRE(large);
    CHECK(large->rgba != flat->rgba);
    // More distinct materials than slots: the surplus stays flat and nothing overflows the range.
    auto many = model;
    many.materialNames.clear();
    many.submeshes.clear();
    many.skins = {{}};
    ModelMaterials lots;
    for (std::int32_t i = 0; i < 80; ++i) {
        many.materialNames.push_back("apex/m" + std::to_string(i));
        many.submeshes.push_back({static_cast<std::uint32_t>((i % 6) * 6), 6, 0, 0, 0, i});
        many.skins[0].push_back(static_cast<std::int16_t>(i));
        lots.emplace(i, ModelMaterial{"apex/m" + std::to_string(i), testing::thumbnailMaterial()});
    }
    auto bulk = (*renderer)->render(many, lots);
    CHECK(bulk.has_value());
    CHECK((*renderer)->maxThumbnailSlotsLive() <= kThumbnailTextureSlots);
}

namespace {
// A scheduler over synthetic models that "load" when polled twice, and synthetic materials.
struct SyntheticSources {
    std::map<std::string, assets::ModelState> models;
    std::map<std::string, assets::MaterialState> materialStates;
    std::shared_ptr<assets::ModelData> data = [] {
        auto model = std::make_shared<assets::ModelData>(testing::thumbnailModel());
        model->materialDirectories = {"models/"};
        return model;
    }();
    std::shared_ptr<const assets::CachedMaterial> material = testing::thumbnailMaterial();
    int modelLoads = 0, materialLoads = 0, renders = 0;
    std::vector<std::size_t> materialCounts;
    std::chrono::microseconds renderCost{0};
    ModelThumbnailScheduler::ModelSource modelSource() {
        return {[this](std::string_view p) { return models[std::string(p)]; },
                [this](std::string_view p) -> std::shared_ptr<const assets::ModelData> {
                    auto& state = models[std::string(p)];
                    if (state == assets::ModelState::Unloaded) { state = assets::ModelState::Loading; ++modelLoads; return {}; }
                    return state == assets::ModelState::Ready ? data : nullptr;
                },
                [](std::string_view) { return std::optional<Error>{}; }};
    }
    ModelThumbnailScheduler::MaterialSource materialSource() {
        return {[this](std::string_view n) {
                    auto found = materialStates.find(std::string(n));
                    return found == materialStates.end() ? assets::MaterialState::Unloaded : found->second;
                },
                [this](std::string_view n) -> std::shared_ptr<const assets::CachedMaterial> {
                    auto& state = materialStates[std::string(n)];
                    if (state == assets::MaterialState::Unloaded) { state = assets::MaterialState::Loading; ++materialLoads; return {}; }
                    return material;
                },
                [](const std::string&) { return true; }};
    }
    ModelThumbnailScheduler::Render render() {
        return [this](const assets::ModelData&, const ModelMaterials& loaded) -> Result<assets::Thumbnail> {
            ++renders;
            materialCounts.push_back(loaded.size());
            const auto until = std::chrono::steady_clock::now() + renderCost;
            while (std::chrono::steady_clock::now() < until) {}
            return assets::Thumbnail{size, size, std::vector<std::uint8_t>(size * size * 4, 255)};
        };
    }
};
}

TEST_CASE("model thumbnail scheduler starts many loads per tick up to the in-flight cap, visible first") {
    CacheFixture fixture;
    SyntheticSources sources;
    ModelThumbnailScheduler scheduler(*fixture.cache, sources.modelSource(), {}, sources.render());
    scheduler.options().maxLoads = 10;
    for (int i = 0; i < 40; ++i) scheduler.request("off" + std::to_string(i), false);
    for (int i = 0; i < 5; ++i) scheduler.request("vis" + std::to_string(i), true);
    CHECK_FALSE(scheduler.tick(true));
    CHECK(sources.modelLoads == 0);
    CHECK_FALSE(scheduler.tick(false));
    CHECK(sources.modelLoads == 10); // Capped, not one per tick.
    for (int i = 0; i < 5; ++i)
        CHECK(sources.models[assets::modelPath("vis" + std::to_string(i))] == assets::ModelState::Loading);
    CHECK_FALSE(scheduler.tick(false));
    CHECK(sources.modelLoads == 10); // Nothing completed, so nothing more starts.
    for (auto& [path, state] : sources.models)
        if (state == assets::ModelState::Loading) state = assets::ModelState::Ready;
    scheduler.options().tickBudget = {};
    CHECK(scheduler.tick(false)); // Zero budget: one unit of work, the render.
    CHECK(sources.renders == 1);
    CHECK(sources.modelLoads == 10);
    scheduler.options().tickBudget = std::chrono::hours(1);
    CHECK(scheduler.tick(false)); // Ready models are no longer in flight, so ten more start.
    CHECK(sources.modelLoads == 20);
    scheduler.options().tickBudget = {};
    // Drain everything.
    for (int guard = 0; scheduler.pending() && guard < 2000; ++guard) {
        scheduler.tick(false);
        for (auto& [path, state] : sources.models)
            if (state == assets::ModelState::Loading) state = assets::ModelState::Ready;
    }
    CHECK(scheduler.pending() == 0);
    CHECK(sources.renders == 45);
}

TEST_CASE("model thumbnail scheduler renders several thumbnails per tick within its time budget") {
    CacheFixture fixture;
    SyntheticSources sources;
    sources.renderCost = std::chrono::milliseconds(2);
    ModelThumbnailScheduler scheduler(*fixture.cache, sources.modelSource(), {}, sources.render());
    scheduler.options().tickBudget = std::chrono::milliseconds(5);
    for (int i = 0; i < 20; ++i) {
        sources.models[assets::modelPath("m" + std::to_string(i))] = assets::ModelState::Ready;
        scheduler.request("m" + std::to_string(i));
    }
    CHECK(scheduler.tick(false));
    CHECK(sources.renders >= 2);
    CHECK(sources.renders <= 4); // 2 ms each against 5 ms, plus the one that crosses the line.
    scheduler.options().tickBudget = {};
    const int before = sources.renders;
    CHECK(scheduler.tick(false));
    CHECK(sources.renders == before + 1);
    scheduler.options().tickBudget = std::chrono::hours(1);
    CHECK(scheduler.tick(false));
    CHECK(scheduler.pending() == 0);
    CHECK(sources.renders == 20);
}

TEST_CASE("model thumbnail scheduler waits for materials, then renders flat for missing or failed ones") {
    CacheFixture fixture;
    SyntheticSources sources;
    ModelThumbnailScheduler scheduler(*fixture.cache, sources.modelSource(), sources.materialSource(), sources.render());
    scheduler.options().tickBudget = {};
    scheduler.options().materialWaitTicks = 5;
    const auto name = [](const char* n) { return assets::modelPath(n); };
    sources.models[name("textured")] = assets::ModelState::Ready;
    scheduler.request("textured");
    // thumbnailModel's skin 0 resolves to "models/apex/thumbnail" (material 0, the only drawn range).
    CHECK_FALSE(scheduler.tick(false));
    CHECK(sources.materialLoads == 1);
    CHECK(sources.renders == 0);
    CHECK_FALSE(scheduler.tick(false));
    CHECK(sources.materialLoads == 1); // Requested once.
    sources.materialStates["models/apex/thumbnail"] = assets::MaterialState::Ready;
    CHECK(scheduler.tick(false));
    REQUIRE(sources.materialCounts.size() == 1);
    CHECK(sources.materialCounts[0] == 1);
    // A failed material renders at once, without it.
    sources.materialStates["models/apex/thumbnail"] = assets::MaterialState::Failed;
    sources.models[name("failed")] = assets::ModelState::Ready;
    scheduler.request("failed");
    CHECK(scheduler.tick(false));
    CHECK(sources.materialCounts.back() == 0);
    // One that never finishes is given up on after the wait budget.
    sources.materialStates["models/apex/thumbnail"] = assets::MaterialState::Loading;
    sources.models[name("stuck")] = assets::ModelState::Ready;
    scheduler.request("stuck");
    int ticks = 0;
    while (!scheduler.tick(false) && ticks < 20) ++ticks;
    CHECK(ticks >= 4);
    CHECK(ticks < 10);
    CHECK(sources.materialCounts.back() == 0);
    CHECK(scheduler.pending() == 0);
}

// Opt-in (APEX_P2CE_DIR and a Vulkan device). Prints throughput of the real pipeline for a screenful
// of models, against the serial behaviour this scheduler replaced (one load, one flat render per tick).
TEST_CASE("model thumbnail corpus throughput") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    assets::GameProfile profile;
    profile.gameDirectory = std::filesystem::path(install) / "p2ce";
    profile.addons.enabled = false;
    JobPool jobs(4);
    auto loaded = assets::Vfs::load(profile, jobs); REQUIRE(loaded);
    const assets::Vfs& vfs = *loaded;
    assets::ModelIndex index(vfs, jobs);
    while (!index.progress().finished) { index.step(); jobs.drainCompletions(); std::this_thread::yield(); }
    auto snapshot = index.snapshot(); REQUIRE(snapshot);
    constexpr std::size_t count = 500;
    std::vector<std::string> names;
    const auto stride = std::max<std::size_t>(1, snapshot->entries.size() / count);
    for (std::size_t i = 0; i < snapshot->entries.size() && names.size() < count; i += stride)
        names.push_back(snapshot->entries[i].name);
    auto heap = TextureHeap::create(*context); REQUIRE(heap);
    auto renderer = ModelThumbnailRenderer::create(*context, **heap); REQUIRE(renderer);
    struct Result { double wallMs = 0, maxTickMs = 0; int ticks = 0, ok = 0, failed = 0; };
    const auto run = [&](bool textured, std::size_t maxLoads, std::chrono::microseconds budget) {
        assets::ModelCache models(vfs, jobs);
        assets::MaterialCache materials(vfs, jobs);
        assets::ThumbnailCache cache(vfs, jobs, nullptr);
        const auto exists = [&](const std::string& path) { return vfs.exists(path); };
        std::unique_ptr<ModelThumbnailScheduler> scheduler = textured
            ? std::make_unique<ModelThumbnailScheduler>(models, materials, exists, cache, **renderer)
            : std::make_unique<ModelThumbnailScheduler>(models, cache, **renderer);
        scheduler->options().maxLoads = maxLoads;
        scheduler->options().tickBudget = budget;
        for (const auto& name : names) scheduler->request(name, true);
        Result result;
        const auto begin = std::chrono::steady_clock::now();
        while (scheduler->pending() && result.ticks < 100000) {
            const auto tick = std::chrono::steady_clock::now();
            scheduler->tick(false);
            result.maxTickMs = std::max(result.maxTickMs, std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tick).count());
            jobs.drainCompletions();
            ++result.ticks;
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Stand-in for the UI timer gap.
        }
        result.wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        for (const auto& name : names)
            (cache.state(kind, name, size) == assets::ThumbnailState::Ready ? result.ok : result.failed)++;
        return result;
    };
    const auto serial = run(false, 1, {});
    const auto flat = run(false, 24, std::chrono::milliseconds(8));
    const auto textured = run(true, 24, std::chrono::milliseconds(8));
    const auto print = [](const char* label, const Result& r) {
        std::cout << "[model thumbnails x" << count << "] " << label << ": " << r.wallMs << " ms wall, " << r.ticks
                  << " ticks (x16 ms timer = " << r.ticks * 16 << " ms), max tick " << r.maxTickMs << " ms, "
                  << r.ok << " ok / " << r.failed << " failed\n";
    };
    print("serial flat (old)", serial);
    print("parallel flat", flat);
    print("parallel textured", textured);
    CHECK(textured.ok > 0);
    CHECK(textured.ticks < serial.ticks);
}
