#include "assets/addons.hpp"
#include "assets/model_data.hpp"
#include <mdlpp/structs/MDL.h>
#include "assets/material_cache.hpp"
#include "assets/vfs.hpp"
#include "core/kv3.hpp"

#include <cstdlib>
#include <doctest/doctest.h>
#include <fstream>
#include <thread>
#include <vpkpp/format/VPK.h>

using namespace apex;
using namespace apex::assets;
namespace fs = std::filesystem;
namespace {
void parses(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    REQUIRE_MESSAGE(input, file.string());
    std::string bytes(std::istreambuf_iterator<char>{input}, {});
    auto doc = kv3::parse(bytes);
    REQUIRE_MESSAGE(doc, file.string(), ": ", (doc ? "" : doc.error().what()));
}
}
TEST_CASE("addons kv3 corpus manifests lists and mounted game VPKs are readable") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    const auto root = fs::canonical(install);
    std::size_t manifests = 0, lists = 0, mounted = 0, vpks = 0;
    for (const auto& directory : {root / "p2ce/addons",
                                 root.parent_path().parent_path() / "workshop/content/440000"}) {
        if (!fs::exists(directory)) continue;
        for (const auto& child : fs::directory_iterator(directory)) {
            if (!child.is_directory()) continue;
            const auto manifest = child.path() / "addon.kv3";
            if (fs::exists(manifest)) { parses(manifest); ++manifests; }
        }
    }
    for (const auto* path : {"p2ce/addons/addonlist.kv3", "hammer/addons/addonlist.kv3"})
        if (fs::exists(root / path)) { parses(root / path); ++lists; }
    GameProfile profile;
    profile.gameDirectory = root / "p2ce";
    profile.addons.list = AddonSettings::List::Game;
    auto list = readAddonList(profile);
    REQUIRE_MESSAGE(list, (list ? "" : list.error().what()));
    for (const auto& entry : list->entries) {
        if (!entry.mounted) continue;
        ++mounted;
        REQUIRE_MESSAGE(!entry.resolved.empty(), entry.dir);
        for (const auto& name : entry.vpks) {
            const auto file = entry.resolved / name;
            auto archive = vpkpp::VPK::open(file.string());
            CHECK_MESSAGE(archive, file.string());
            ++vpks;
        }
    }
    MESSAGE("Addon corpus: ", manifests, " manifests, ", lists, " lists, ", mounted, " mounted entries, ", vpks, " VPKs");
}

TEST_CASE("addons corpus Workshop materials load by default and vanish when unmounted") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile profile;
    profile.gameDirectory = fs::canonical(install) / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    // One material per mounted Workshop addon, taken from its own archive.
    std::vector<std::pair<std::size_t, std::string>> samples;
    const auto& entries = vfs->addons().entries;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].mounted || entries[i].type != "steam" || entries[i].vpks.empty()) continue;
        auto archive = vpkpp::VPK::open((entries[i].resolved / entries[i].vpks.front()).string());
        REQUIRE(archive);
        std::string vmt;
        archive->runForAllEntries([&](const std::string& path, const vpkpp::Entry&) {
            if (vmt.empty() && path.starts_with("materials/") && path.ends_with(".vmt") && !path.starts_with("materials/models/"))
                vmt = path;
        });
        if (!vmt.empty()) samples.emplace_back(i, vmt);
    }
    if (samples.empty()) { MESSAGE("Skipped: no mounted Workshop addon in the game list"); return; }
    MaterialCache cache(*vfs, jobs);
    for (const auto& [index, vmt] : samples) {
        CAPTURE(vmt);
        const auto provider = vfs->provider(vmt);
        REQUIRE(provider);
        CHECK(entries[*provider].mounted);
        const auto name = vmt.substr(10, vmt.size() - 14);
        cache.request(name);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (cache.state(name) == MaterialState::Loading && std::chrono::steady_clock::now() < deadline) {
            jobs.drainCompletions();
            std::this_thread::yield();
        }
        CHECK_MESSAGE(cache.state(name) == MaterialState::Ready, (cache.error(name) ? cache.error(name)->what() : ""));
    }
    // Mount only the local addons: no Workshop sample may come from a Workshop addon any more.
    profile.addons.list = AddonSettings::List::Apex;
    for (const auto& entry : entries)
        if (entry.mounted && entry.type == "local" && !entry.resolved.empty()) profile.addons.apexMounted.push_back(entry.resolved);
    auto local = Vfs::load(profile, jobs);
    REQUIRE(local);
    for (const auto& [index, vmt] : samples) {
        CAPTURE(vmt);
        const auto provider = local->provider(vmt);
        CHECK((!provider || local->addons().entries[*provider].type == "local"));
    }
    MESSAGE("Workshop materials: ", samples.size(), " addons sampled");
}

TEST_CASE("addons corpus game list toggles byte for byte on a copy") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    const auto source = fs::canonical(install) / "p2ce/addons/addonlist.kv3";
    if (!fs::exists(source)) { MESSAGE("Skipped: no game addon list"); return; }
    const auto read = [](const fs::path& file) {
        std::ifstream input(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{input}, {});
    };
    const auto original = read(source);
    const auto temp = fs::temp_directory_path() / ("apex-addonlist-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp / "p2ce/addons");
    fs::copy_file(source, temp / "p2ce/addons/addonlist.kv3");
    GameProfile profile;
    profile.gameDirectory = temp / "p2ce";
    auto list = readAddonList(profile);
    REQUIRE(list);
    for (std::size_t i = 0; i < list->entries.size(); ++i) {
        const bool mounted = list->entries[i].mounted;
        REQUIRE(setAddonMounted(*list, i, !mounted));
        auto flipped = readAddonList(profile);
        REQUIRE(flipped);
        CHECK(flipped->entries[i].mounted == !mounted);
        CHECK(read(temp / "p2ce/addons/addonlist.kv3").size() == original.size() + (mounted ? 1 : std::size_t(-1)));
        REQUIRE(setAddonMounted(*flipped, i, mounted));
        CHECK(read(temp / "p2ce/addons/addonlist.kv3") == original);
    }
    fs::remove_all(temp);
    CHECK(read(source) == original);
}

TEST_CASE("EverythingPBR door model materials resolve and decode from the mounted pack") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile profile;
    profile.gameDirectory = fs::canonical(install) / "p2ce";
    profile.addons.list = AddonSettings::List::Game;
    JobPool jobs(2);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    constexpr auto modelPath = "models/pbr/test_chambers/l2_testchmb_door.mdl";
    if (!vfs->exists(modelPath)) { MESSAGE("Skipped: EverythingPBR is not mounted"); return; }
    const auto bytes = vfs->open(modelPath);
    REQUIRE(bytes);
    mdlpp::MDL::MDL mdl;
    REQUIRE(mdl.open(bytes->data(), bytes->size()));
    ModelData model;
    model.materialDirectories = mdl.materialDirectories;
    for (const auto& material : mdl.materials) model.materialNames.push_back(material.name);
    REQUIRE_FALSE(model.materialNames.empty());
    MaterialCache cache(*vfs, jobs);
    for (std::size_t i = 0; i < model.materialNames.size(); ++i) {
        CAPTURE(model.materialNames[i]);
        const auto name = resolveModelMaterial(model, static_cast<std::int32_t>(i),
            [&](const std::string& path) { return vfs->exists(path); });
        REQUIRE_FALSE(name.empty());
        cache.request(name);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (cache.state(name) == MaterialState::Loading && std::chrono::steady_clock::now() < deadline) {
            jobs.drainCompletions();
            std::this_thread::yield();
        }
        CHECK_MESSAGE(cache.state(name) == MaterialState::Ready,
                      (cache.error(name) ? cache.error(name)->what() : ""));
    }
}
