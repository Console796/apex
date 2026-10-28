// 0.4 P0 confirmations: kvpp's DMX reader opens every P2CE particle file, and bsppp reads a
// compiled map's entity lump (the release gate's way of checking Apex-built maps).
#include "assets/vfs.hpp"
#include "fixture.hpp"

#include <bsppp/bsppp.h>
#include <doctest/doctest.h>
#include <kvpp/kvpp.h>

#include <cstdlib>
#include <filesystem>
#include <map>

using namespace apex;
using namespace apex::assets;

TEST_CASE("kvpp DMX opens every particle file the P2CE mount lists") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile profile;
    profile.gameDirectory = std::filesystem::path(install) / "p2ce";
    JobPool jobs(1);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    const auto files = vfs->walk("particles", "*.pcf");
    REQUIRE_FALSE(files.empty());
    std::size_t failed = 0, systems = 0;
    for (const auto& path : files) {
        CAPTURE(path);
        const auto blob = vfs->open(path);
        REQUIRE(blob);
        kvpp::DMX dmx(std::span<const std::byte>(blob->data(), blob->size()));
        const bool ok = static_cast<bool>(dmx);
        failed += !ok;
        CHECK(ok);
        if (ok && !dmx.getElements().empty()) {
            for (const auto& attribute : dmx.getElements().front().getAttributes()) {
                if (attribute.getKey() == "particleSystemDefinitions") { ++systems; break; }
            }
        }
    }
    MESSAGE("pcf files=" << files.size() << " opened=" << files.size() - failed
                         << " with particleSystemDefinitions=" << systems);
}

TEST_CASE("bsppp reads the entity lump of every stock P2CE map it is given") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    const auto maps = std::filesystem::path(install) / "p2ce" / "maps";
    std::size_t read = 0, withOutputs = 0, entities = 0;
    for (const auto& file : std::filesystem::directory_iterator(maps)) {
        if (file.path().extension() != ".bsp") continue;
        CAPTURE(file.path().string());
        bsppp::BSP bsp(file.path().string());
        REQUIRE(static_cast<bool>(bsp));
        const auto list = bsp.getLumpData<bsppp::BSPLump::ENTITIES>();
        REQUIRE_FALSE(list.empty());
        ++read;
        entities += list.size();
        bool hasWorld = false, hasOutput = false;
        for (const auto& entity : list) {
            for (const auto& pair : entity.getKeyValues())
                if (pair.getKey() == "classname" && pair.getValue() == "worldspawn") hasWorld = true;
            if (entity.getKeyValuesCount("OnPressed") || entity.getKeyValuesCount("OnTrigger")) hasOutput = true;
        }
        withOutputs += hasOutput;
        CHECK(hasWorld);
    }
    MESSAGE("bsp files=" << read << " entities=" << entities << " maps with an OnPressed/OnTrigger output=" << withOutputs);
    CHECK(read > 0);
}
