#include "assets/model_index.hpp"
#include "assets/sound_index.hpp"
#include "assets/particle_index.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace apex;
using namespace apex::assets;
namespace {
template <typename Kind> auto buildIndex(const Vfs& vfs, JobPool& jobs, const char* label) {
    const auto started = std::chrono::steady_clock::now();
    AssetIndex<Kind> index(vfs, jobs);
    while (!index.progress().finished && std::chrono::steady_clock::now() - started < std::chrono::seconds(120)) {
        index.step(); jobs.drainCompletions(); std::this_thread::yield();
    }
    REQUIRE(index.progress().finished);
    auto snapshot = index.snapshot(); REQUIRE(snapshot);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << label << " index: " << snapshot->entries.size() << " entries in " << ms << " ms\n";
    return snapshot;
}
}
TEST_CASE("asset index corpus records P2CE model sound and particle counts and time") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile profile; profile.gameDirectory = std::filesystem::path(install) / "p2ce";
    // Stock P2CE mount set, independent of personal Workshop/local addon manifests.
    // Synthetic tests cover addon overrides and provider indices.
    profile.addons.enabled = false;
    JobPool jobs(2); auto vfs = Vfs::load(profile, jobs); REQUIRE(vfs);
    auto models = buildIndex<ModelKind>(*vfs, jobs, "Model");
    CHECK(models->entries.size() > 1800); CHECK(models->entries.size() < 2800);
    for (const auto& entry : models->entries) { CAPTURE(entry.name); CHECK(entry.error.empty()); }
    auto sounds = buildIndex<SoundKind>(*vfs, jobs, "Sound");
    std::size_t raw = 0, scripts = 0, errors = 0;
    for (const auto& entry : sounds->entries) {
        if (entry.type == SoundIndexEntry::Type::Raw) ++raw;
        else { ++scripts; CHECK_FALSE((entry.waves.empty() && entry.error.empty())); }
        errors += !entry.error.empty();
    }
    CHECK(raw > 0); CHECK(scripts > 0);
    std::cout << "Sound types: raw=" << raw << " soundscript=" << scripts << " errors=" << errors << '\n';
    auto particles = buildIndex<ParticleKind>(*vfs, jobs, "Particle");
    REQUIRE_FALSE(particles->entries.empty());
    std::set<std::string> pcfs;
    for (const auto& entry : particles->entries) {
        CAPTURE(entry.pcf); CAPTURE(entry.name); CHECK(entry.error.empty()); pcfs.insert(entry.pcf);
    }
    std::cout << "Particle manifest PCFs with definitions=" << pcfs.size() << '\n';
}
