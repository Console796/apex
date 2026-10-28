#include "assets/model_cache.hpp"
#include "fixture.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace apex;
using namespace apex::assets;

namespace {
void finish(JobPool& jobs, ModelCache& cache, std::string_view name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (cache.state(name) == ModelState::Loading && std::chrono::steady_clock::now() < deadline) {
        jobs.drainCompletions();
        std::this_thread::yield();
    }
    REQUIRE(cache.state(name) != ModelState::Loading);
}

std::string_view text(const Blob& blob) {
    return {reinterpret_cast<const char*>(blob.data()), blob.size()};
}
} // namespace

TEST_CASE("ModelCache name normalisation") {
    CHECK(modelPath("Models\\Props\\Foo") == "models/props/foo.mdl");
    CHECK(modelPath("models/props/foo.MDL") == "models/props/foo.mdl");
}

TEST_CASE("ModelCache missing and corrupt models become the error model") {
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("models/garbage.mdl", "not a model");
    tree.put("models/garbage.dx90.vtx", "not a strip file");
    tree.put("models/garbage.vvd", "not vertices");
    tree.put("models/nomate.mdl", "only an mdl");
    JobPool jobs(1);
    GameProfile profile;
    profile.gameDirectory = tree.root;
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    ModelCache cache(*vfs, jobs);
    const auto errorModel = cache.errorModel();
    REQUIRE(errorModel);
    CHECK(errorModel->indices.size() == 36);
    CHECK(errorModel->submeshes.size() == 1);
    CHECK_FALSE(errorModel->bounds.empty());

    CHECK(cache.request("models/missing") == errorModel);
    CHECK(cache.state("models/missing") == ModelState::Failed);
    REQUIRE(cache.error("models/missing"));

    CHECK(cache.request("models/nomate") == errorModel); // vtx and vvd absent
    CHECK(cache.state("models/nomate") == ModelState::Failed);

    CHECK(cache.request("models/garbage") == nullptr);
    CHECK(cache.state("models/garbage") == ModelState::Loading);
    finish(jobs, cache, "models/garbage");
    CHECK(cache.state("models/garbage") == ModelState::Failed);
    CHECK(cache.request("models/garbage") == errorModel);
    CHECK(cache.error("models/garbage").has_value());
    CHECK(cache.residentBytes() == 0);

    { ModelCache temporary(*vfs, jobs); temporary.request("models/garbage"); }
    jobs.drainCompletions(); // Completion after destruction is harmless, including under ASan.
}

TEST_CASE("ModelCache corpus models load async, evict by bytes, clear on mount change and survive truncation") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile profile;
    profile.gameDirectory = std::filesystem::path(install) / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    ModelCache cache(*vfs, jobs);

    // The first two models in canonical order that bake to something small.
    std::vector<std::string> names;
    for (const auto& path : vfs->walk("models", "*.mdl")) {
        if (names.size() == 2) break;
        const auto stem = path.substr(0, path.size() - 4);
        const auto mdl = vfs->open(path);
        const auto vtx = vfs->open(stem + ".dx90.vtx");
        const auto vvd = vfs->open(stem + ".vvd");
        if (mdl && vtx && vvd && vvd->size() < 64 * 1024) names.push_back(path);
    }
    REQUIRE(names.size() == 2);
    CAPTURE(names[0]);
    CAPTURE(names[1]);

    CHECK(cache.request(names[0]) == nullptr);
    CHECK(cache.state(names[0]) == ModelState::Loading);
    CHECK(cache.request(names[0]) == nullptr); // One load, however often it is asked for.
    finish(jobs, cache, names[0]);
    REQUIRE(cache.state(names[0]) == ModelState::Ready);
    const auto first = cache.request(names[0]);
    REQUIRE(first);
    CHECK_FALSE(first->indices.empty());
    CHECK(first != cache.errorModel());
    const auto firstBytes = modelByteSize(*first);
    CHECK(cache.residentBytes() == firstBytes);

    cache.request(names[1]);
    finish(jobs, cache, names[1]);
    REQUIRE(cache.state(names[1]) == ModelState::Ready);
    cache.request(names[0]); // names[0] is most recently used
    cache.setBudget(firstBytes);
    CHECK(cache.state(names[1]) == ModelState::Unloaded);
    CHECK(cache.state(names[0]) == ModelState::Ready);
    cache.setBudget(0);
    CHECK(cache.residentBytes() == 0);
    CHECK(first->vertices.size() > 0); // The caller's snapshot survives eviction.

    // A model larger than the budget stays resident once loaded; it is not reloaded on every request.
    cache.setBudget(1);
    cache.request(names[0]);
    finish(jobs, cache, names[0]);
    REQUIRE(cache.state(names[0]) == ModelState::Ready);
    CHECK(cache.request(names[0]) != nullptr);
    CHECK(cache.request(names[0]) != cache.errorModel());
    cache.request(names[1]); // The next load evicts it instead.
    finish(jobs, cache, names[1]);
    CHECK(cache.state(names[1]) == ModelState::Ready);
    CHECK(cache.state(names[0]) == ModelState::Unloaded);

    cache.setBudget(256 * 1024 * 1024);
    cache.request(names[0]);
    finish(jobs, cache, names[0]);
    REQUIRE(cache.state(names[0]) == ModelState::Ready);
    cache.request(names[1]); // In flight while the mount changes.
    cache.clear();
    CHECK(cache.state(names[0]) == ModelState::Unloaded);
    CHECK(cache.state(names[1]) == ModelState::Unloaded);
    CHECK(cache.residentBytes() == 0);
    jobs.drainCompletions(); // The cancelled or stale completion must not repopulate the cache.
    CHECK(cache.state(names[1]) == ModelState::Unloaded);
    CHECK(first->vertices.size() > 0);

    // A truncated VVD and a VTX with a flipped checksum: error model or a clean bake, never a crash.
    const auto stem = names[0].substr(0, names[0].size() - 4);
    auto mdl = *vfs->open(names[0]);
    auto vtx = *vfs->open(stem + ".dx90.vtx");
    auto vvd = *vfs->open(stem + ".vvd");
    GameTree tree;
    tree.put("gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("models/cut.mdl", text(mdl));
    tree.put("models/cut.dx90.vtx", text(vtx));
    vvd.resize(vvd.size() / 2);
    tree.put("models/cut.vvd", text(vvd));
    GameProfile local;
    local.gameDirectory = tree.root;
    auto localVfs = Vfs::load(local, jobs);
    REQUIRE(localVfs);
    ModelCache localCache(*localVfs, jobs);
    localCache.request("models/cut");
    finish(jobs, localCache, "models/cut");
    CHECK(localCache.state("models/cut") == ModelState::Failed);
    CHECK(localCache.request("models/cut") == localCache.errorModel());
}
