#include "render/accel.hpp"
#include "shared_context.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>

using namespace apex::render;

TEST_CASE("ray query: measure synchronous TLAS build and update for 4096 instances") {
    const char* enabled = std::getenv("APEX_MEASURE");
    if (!enabled || std::string_view(enabled) != "1") { MESSAGE("APEX_MEASURE=1 is required"); return; }
    auto* context = testing::sharedContext();
    if (!context) { MESSAGE("Skipped: no Vulkan device"); return; }
    MESSAGE("Ray-query device: " << context->info().name << "; validation=" << context->info().validation);
    if (!context->info().rayQuery) { MESSAGE("Skipped: ray query is not enabled on this device"); return; }
    context->validation().clear();
    {
        const std::array<std::array<float, 3>, 3> positions{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        auto blas = Accel::buildBlas(*context, positions, indices);
        REQUIRE(blas);
        std::vector<AccelInstance> instances(4096);
        for (std::size_t i = 0; i < instances.size(); ++i) {
            instances[i].blas = blas->get();
            instances[i].customIndex = static_cast<std::uint32_t>(i);
            instances[i].transform.matrix[0][3] = float(i % 64) * 2;
            instances[i].transform.matrix[1][3] = float(i / 64) * 2;
        }
        using Clock = std::chrono::steady_clock;
        std::vector<double> builds, updates;
        for (int sample = 0; sample < 8; ++sample) {
            const auto start = Clock::now();
            auto tlas = Accel::buildTlas(*context, instances);
            const auto built = Clock::now();
            REQUIRE(tlas);
            instances[0].transform.matrix[2][3] = float(sample + 1);
            const auto updateStart = Clock::now();
            REQUIRE((*tlas)->updateInstances(instances));
            const auto end = Clock::now();
            if (sample == 0) continue; // Warm up driver/pipeline caches.
            builds.push_back(std::chrono::duration<double, std::milli>(built - start).count());
            updates.push_back(std::chrono::duration<double, std::milli>(end - updateStart).count());
        }
        std::ranges::sort(builds);
        std::ranges::sort(updates);
        MESSAGE("4096 instances, 7 samples: build median_ms=" << builds[3] << " min_ms=" << builds.front()
                << " max_ms=" << builds.back() << "; update median_ms=" << updates[3]
                << " min_ms=" << updates.front() << " max_ms=" << updates.back());
    }
    for (const auto& error : context->validation().errors()) MESSAGE(error);
    CHECK(context->validation().errorCount() == 0);
    CHECK(context->validation().warningCount() == 0);
}
