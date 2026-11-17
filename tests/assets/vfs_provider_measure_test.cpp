// Plan plugins.md probe P3 (0.9.6): a provider mounted between profile mounts, with Vfs::walkTask
// running on workers. Three loose profile mounts of kFiles VMTs each, and a provider of kFiles
// entries mounted between the second and third. Reports load time with and without the provider,
// walk time (4 concurrent walkTasks on a JobPool, repeated), entry counts, whether "earlier mounts
// win" holds, and which thread ran the provider's refresh when the load itself runs on a worker
// (as GameContent::reload does). Opt-in (APEX_MEASURE=1), in apex_measure_tests; it reports.
#include "assets/vfs.hpp"
#include "fixture.hpp"
#include "provider_fixture.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

using namespace apex;
using namespace apex::assets;

namespace {
using Clock = std::chrono::steady_clock;
constexpr int kFiles = 3000;

bool measureEnabled() {
    const auto* value = std::getenv("APEX_MEASURE");
    return value && std::string_view(value) == "1";
}
template <typename F> double milliseconds(F&& operation) {
    const auto start = Clock::now();
    operation();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct ThreadProvider final : MemoryProvider {
    using MemoryProvider::MemoryProvider;
    apex::Result<void> refresh() override {
        refreshThread = std::this_thread::get_id();
        return MemoryProvider::refresh();
    }
    std::thread::id refreshThread;
};
} // namespace

TEST_CASE("[measure] 0.9.6 P3 provider mounted between profile mounts, walkTask on workers") {
    if (!measureEnabled()) {
        MESSAGE("APEX_MEASURE=1 is required");
        return;
    }
    GameTree tree;
    tree.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths {
        game "First" game "|gameinfo_path|." game "Last" } } })");
    for (int i = 0; i < kFiles; ++i) {
        const auto name = "materials/set/m" + std::to_string(i) + ".vmt";
        tree.put("First/" + name, "first");
        tree.put("Game/" + name, "game");
        tree.put("Last/" + name, "last");
    }
    auto provider = std::make_shared<ThreadProvider>("probe");
    for (int i = 0; i < kFiles; ++i) {
        provider->contents["materials/set/m" + std::to_string(i) + ".vmt"] = "provider";
        provider->contents["materials/plugin/p" + std::to_string(i) + ".vmt"] = "provider";
    }
    GameProfile profile;
    profile.gameDirectory = tree.root / "Game";
    JobPool jobs(4);

    std::optional<Vfs> plain, withProvider;
    const double loadPlain = milliseconds([&] { plain.emplace(std::move(*Vfs::load(profile, jobs))); });
    const double loadProvider = milliseconds([&] {
        withProvider.emplace(std::move(*Vfs::load(profile, jobs, {},
            {{provider, {VfsPosition::Kind::Before, "/Last"}}})));
    });

    const auto walkMs = [&](Vfs& vfs, std::size_t& count) {
        constexpr int repetitions = 10;
        const auto start = Clock::now();
        for (int rep = 0; rep < repetitions; ++rep) {
            std::vector<std::future<std::size_t>> pending;
            for (int w = 0; w < 4; ++w) {
                auto task = vfs.walkTask("materials", "*.vmt");
                pending.push_back(std::async(std::launch::async, [task] { return task().size(); }));
            }
            for (auto& p : pending) count = p.get();
        }
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count() / repetitions;
    };
    std::size_t plainCount = 0, providerCount = 0;
    const double walkPlain = walkMs(*plain, plainCount);
    const double walkProvider = walkMs(*withProvider, providerCount);

    // Worker-thread walks through jobs, and a refresh while no walk runs.
    std::atomic<std::size_t> viaPool{0};
    std::vector<std::future<void>> done;
    for (int w = 0; w < 8; ++w) {
        auto promise = std::make_shared<std::promise<void>>();
        done.push_back(promise->get_future());
        auto task = withProvider->walkTask("materials", "*.vmt");
        jobs.submit(Priority::Normal, [task, promise, &viaPool](CancelToken) {
            viaPool += task().size();
            promise->set_value();
        });
    }
    for (auto& d : done) d.get();
    const double refreshMs = milliseconds([&] { REQUIRE(withProvider->refresh(jobs)); });

    // The same load, run on a worker the way GameContent::reload does.
    auto workerProvider = std::make_shared<ThreadProvider>("worker");
    workerProvider->contents = provider->contents;
    std::promise<std::thread::id> loadThread;
    auto loadThreadFuture = loadThread.get_future();
    std::promise<void> loaded;
    std::optional<Vfs> fromWorker;
    jobs.submit(Priority::Normal, [&](CancelToken) {
        loadThread.set_value(std::this_thread::get_id());
        auto vfs = Vfs::load(profile, jobs, {}, {{workerProvider, {VfsPosition::Kind::Before, "/Last"}}});
        if (vfs) fromWorker.emplace(std::move(*vfs));
        loaded.set_value();
    });
    loaded.get_future().get();
    const bool refreshOnWorker = workerProvider->refreshThread == loadThreadFuture.get();

    const bool precedence = asText(*withProvider->open("materials/set/m7.vmt")) == "first" &&
                            asText(*withProvider->open("materials/plugin/p7.vmt")) == "provider";
    std::cout << "[P3] files per mount " << kFiles << ", provider " << provider->contents.size()
              << "\n[P3] load without provider " << loadPlain << " ms, with " << loadProvider
              << " ms\n[P3] walk (4 concurrent tasks) without " << walkPlain << " ms (" << plainCount
              << " files), with " << walkProvider << " ms (" << providerCount << " files)\n[P3] 8 pool walks counted "
              << viaPool.load() << ", refresh " << refreshMs << " ms, provider opens in the whole run (one probe) "
              << provider->opens.load() << "\n[P3] load on a worker: refresh() ran on the load thread = "
              << (refreshOnWorker ? "yes" : "no") << ", earlier mounts win = " << (precedence ? "yes" : "no")
              << std::endl;
    CHECK(precedence);
    CHECK(fromWorker.has_value());
    CHECK(refreshOnWorker);
    CHECK(providerCount == plainCount + kFiles);
    CHECK(provider->opens == 1); // the one probe that reached the provider (m7 resolves to First)
}
