#include "document/document.hpp"
#include "document/vmf.hpp"
#include "editor/bake_instances.hpp"
#include "lighting/bake_session.hpp"
#include "render/bake_setup.hpp"
#include "render/lightmap_atlas.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>

using namespace apex;
using Clock = std::chrono::steady_clock;

namespace {
double settle(lighting::BakeSession& session) {
    const auto start = Clock::now();
    while (!session.progress().idle) session.advance(1u << 16);
    return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Timings { double visible = 0, direct = 0, dirty = 0, idle = 0; };
// Runs to idle, noting when (a) any of `faces` first has a completed direct pass, (b) all of them have finished
// every level through bounce 1, (c) all of them have reached the maximum at every level.
Timings run(lighting::BakeSession& session, const std::vector<std::size_t>& faces, unsigned maxPasses, unsigned levels) {
    Timings t;
    bool visible = false, direct = false, converged = false;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };
    const auto all = [&](unsigned top) {
        for (unsigned l = 0; l <= top; ++l) for (const auto f : faces) if (session.facePasses(l)[f] < maxPasses) return false;
        return true;
    };
    while (!session.progress().idle) {
        session.advance(1u << 16);
        if (!visible) for (const auto f : faces) if (session.facePasses(0)[f]) { visible = true; t.visible = now(); break; }
        if (!direct && all(std::min(1u, levels - 1))) { direct = true; t.direct = now(); }
        if (!converged && all(levels - 1)) { converged = true; t.dirty = now(); }
    }
    t.idle = now();
    return t;
}
} // namespace

TEST_CASE("incremental bake: measure re-convergence after moving one point light on test1.vmf") {
    const char* enabled = std::getenv("APEX_MEASURE");
    if (!enabled || std::string_view(enabled) != "1") { MESSAGE("APEX_MEASURE=1 is required"); return; }
    const char* home = std::getenv("HOME");
    const std::filesystem::path path = std::filesystem::path(home ? home : ".") / "maps/apex/test1.vmf";
    if (!std::filesystem::exists(path)) { MESSAGE("Skipped: " << path << " missing"); return; }
    std::ostringstream text;
    text << std::ifstream(path).rdbuf();
    auto parsed = vmf::parse(text.str());
    REQUIRE(parsed);
    document::Document doc(*parsed);
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install) { MESSAGE("Skipped: APEX_P2CE_DIR unset"); return; }
    editor::expandBakeInstances(doc, path, std::filesystem::path(install) / "sdk_content" / "maps");
    auto setup = render::setupBake(doc, nullptr);
    auto packed = render::LightmapAtlas::pack(setup.scene.faces);
    REQUIRE(packed);
    const auto keys = packed->faceKeys();
    // First point light, and the point light with the smallest reach (a typical room light).
    std::vector<std::size_t> candidates;
    std::size_t first = setup.scene.lights.size(), smallest = first;
    double smallestRange = 0;
    for (std::size_t i = 0; i < setup.scene.lights.size(); ++i) {
        const auto& l = setup.scene.lights[i];
        if (l.kind != lighting::LightKind::Point) continue;
        const auto range = lighting::effectiveRange(l, lighting::kLightThreshold);
        std::cout << "[incremental bake] point light " << i << " at " << l.origin.x << " " << l.origin.y << " " << l.origin.z
                  << " reach " << range << "\n";
        if (first == setup.scene.lights.size()) first = i;
        if (smallest == setup.scene.lights.size() || range < smallestRange) { smallest = i; smallestRange = range; }
    }
    REQUIRE(first < setup.scene.lights.size());
    candidates = {first};
    if (smallest != first) candidates.push_back(smallest);

    lighting::BakeSessionOptions options;
    options.maxPasses = 16;
    std::size_t light = first;
    const auto moved = [&](double dx) {
        auto scene = setup.scene;
        scene.lights[light].origin.x += dx;
        return scene;
    };
    std::cout << "[incremental bake] faces=" << setup.scene.faces.size() << " lights=" << setup.scene.lights.size()
              << " bounces=" << options.baker.bounces << " maxPasses=" << options.maxPasses << "\n";

    lighting::BakeSession base(setup.scene, options, 1, keys);
    const auto baseSeconds = settle(base);
    std::cout << "[incremental bake] initial full bake " << baseSeconds << " s\n";
    const unsigned levels = options.baker.bounces + 1;
    for (const auto which : candidates) for (const double dx : {16.0, 128.0}) {
        light = which;
        lighting::BakeSession first(setup.scene, options, 1, keys);
        settle(first);
        lighting::BakeSession incremental(lighting::BakeScene{}, options, 2);
        const auto begin = Clock::now();
        const auto diff = incremental.restart(first.takeState(), moved(dx), options, 2, keys);
        const auto diffSeconds = std::chrono::duration<double>(Clock::now() - begin).count();
        std::vector<std::size_t> dirty;
        for (std::size_t i = 0; i < diff.dirty.size(); ++i) if (diff.dirty[i]) dirty.push_back(i);
        const auto inc = run(incremental, dirty, options.maxPasses, levels);
        lighting::BakeSession full(moved(dx), options, 2, keys);
        const auto ref = run(full, dirty, options.maxPasses, levels);
        std::cout << "[incremental bake] light " << which << " moved " << dx << " units: dirty=" << diff.dirtyCount
                  << " bounceDirty=" << diff.bounceDirtyCount << " of " << setup.scene.faces.size() << " (diff " << diffSeconds
                  << " s)\n    first visible change: incremental " << inc.visible << " s, full " << ref.visible
                  << " s\n    dirty faces direct+bounce1 converged: incremental " << inc.direct << " s, full " << ref.direct
                  << " s\n    dirty faces all levels converged: incremental " << inc.dirty << " s, full " << ref.dirty
                  << " s\n    whole map idle: incremental " << inc.idle << " s, full " << ref.idle << " s\n";
        CHECK(inc.idle > 0);
    }
}
