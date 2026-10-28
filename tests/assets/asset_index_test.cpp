// Apex-authored synthetic index fixtures; no game content is embedded.
#include "assets/model_index.hpp"
#include "assets/sound_index.hpp"
#include "assets/particle_index.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"
#include "fixture.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace apex;
using namespace apex::assets;
namespace {
template <typename Kind> void finish(JobPool& jobs, AssetIndex<Kind>& index) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!index.progress().finished && std::chrono::steady_clock::now() < deadline) {
        index.step(); jobs.drainCompletions(); std::this_thread::yield();
    }
    REQUIRE(index.progress().finished);
    CHECK(index.progress().done == index.progress().total);
    REQUIRE(index.snapshot());
    const auto& entries = index.snapshot()->entries;
    CHECK(std::ranges::is_sorted(entries, [](const auto& a, const auto& b) { return ciLess(a.name, b.name); }));
}
void word(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<char>(value >> (8 * i));
}
std::string modelHeader() {
    std::string bytes(408, '\0');
    bytes.replace(0, 4, "IDST"); word(bytes, 4, 48);
    bytes.replace(12, 13, "Apex-authored"); word(bytes, 76, 408);
    word(bytes, 152, 16); word(bytes, 220, 3); word(bytes, 224, 2); word(bytes, 232, 4);
    for (std::size_t i = 0; i < 3; ++i) {
        word(bytes, 104 + i * 4, std::bit_cast<std::uint32_t>(-1.0f - static_cast<float>(i)));
        word(bytes, 116 + i * 4, std::bit_cast<std::uint32_t>(4.0f + static_cast<float>(i)));
        word(bytes, 128 + i * 4, std::bit_cast<std::uint32_t>(-7.0f));
        word(bytes, 140 + i * 4, std::bit_cast<std::uint32_t>(8.0f));
    }
    return bytes;
}
std::string particleFile() {
    // kvpp's writer is not implemented at the pinned revision. Author a minimal binary
    // DMX v1: element table followed by three empty attribute tables.
    std::string bytes = "<!-- dmx encoding binary 1 format pcf 2 -->\n";
    bytes += '\0';
    const auto appendWord = [&](std::uint32_t value) {
        const auto offset = bytes.size(); bytes.resize(offset + 4); word(bytes, offset, value);
    };
    appendWord(3);
    for (auto pair : {std::pair{"DmeParticleSystemDefinition", "Zulu"},
                      {"DmeParticleOperator", "NotASystem"}, {"DmeParticleSystemDefinition", "alpha"}}) {
        bytes += pair.first; bytes += '\0'; bytes += pair.second; bytes += '\0'; bytes.append(16, '\0');
    }
    for (int i = 0; i < 3; ++i) appendWord(0);
    return bytes;
}
void addon(GameTree& tree) {
    constexpr auto header = "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n";
    tree.put("Addon/addon.kv3", std::string(header) + "{ mod = \"Index fixture\" }");
    tree.put("Game/addons/addonlist.kv3", std::string(header) + "{ addons = [ { dir = " +
        json::write({(tree.root / "Addon").string()}) + " mounted = true type = \"local\" vpks = [] } ] }");
}
template <typename Kind> void cancelMidWalk(const Vfs& vfs, JobPool& jobs) {
    AssetIndex<Kind> index(vfs, jobs);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!index.progress().total && std::chrono::steady_clock::now() < deadline) {
        jobs.drainCompletions(); std::this_thread::yield();
    }
    REQUIRE(index.progress().total > 0);
    index.step(std::chrono::milliseconds(0));
    CHECK(index.progress().done == 0);
    index.step(); // cancel with a parse queued or running
    index.cancel();
    for (int i = 0; i < 10; ++i) { jobs.drainCompletions(); index.step(); }
    CHECK_FALSE(index.snapshot());
    CHECK_FALSE(index.progress().finished);
}
}
TEST_CASE("model index header walk snapshot provider and cancellation") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("Game/Models/Zulu.MDL", modelHeader());
    tree.put("Game/models/bad.mdl", "Apex-authored corrupt mdl");
    tree.put("Addon/models/Alpha.mdl", modelHeader()); addon(tree);
    JobPool jobs(2); GameProfile profile; profile.gameDirectory = tree.root / "Game";
    auto vfs = Vfs::load(profile, jobs); REQUIRE(vfs);
    ModelIndex index(*vfs, jobs); CHECK_FALSE(index.snapshot()); finish(jobs, index);
    auto snapshot = index.snapshot(); REQUIRE(snapshot->entries.size() == 3);
    auto entry = snapshot->find("MODELS\\ALPHA.MDL"); REQUIRE(entry);
    CHECK(entry->name == "models/alpha.mdl"); CHECK(entry->error.empty());
    CHECK(entry->flags == 16); CHECK(entry->staticProp); CHECK(entry->bodyPartCount == 4);
    CHECK(entry->skinsPerFamily == 3); CHECK(entry->skinFamilyCount == 2);
    CHECK(entry->hullMin == std::array<float, 3>{-1, -2, -3});
    CHECK(entry->hullMax == std::array<float, 3>{4, 5, 6});
    CHECK(entry->viewMin == std::array<float, 3>{-7, -7, -7});
    CHECK(entry->viewMax == std::array<float, 3>{8, 8, 8});
    CHECK(entry->mount == vfs->provider(entry->name)); CHECK(entry->mount.has_value());
    CHECK_FALSE(snapshot->find("bad")->error.empty()); CHECK_FALSE(snapshot->find("missing"));
    cancelMidWalk<ModelKind>(*vfs, jobs);
}
TEST_CASE("sound index raw and script waves nested rndwave prefixes and errors") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    // Minimal silent mono PCM WAV, authored here; indexing does not decode audio.
    std::string wav(44, '\0'); wav.replace(0, 4, "RIFF"); wav.replace(8, 8, "WAVEfmt ");
    word(wav, 4, 36); word(wav, 16, 16); wav[20] = 1; wav[22] = 1;
    word(wav, 24, 8000); word(wav, 28, 8000); wav[32] = 1; wav[34] = 8; wav.replace(36, 4, "data");
    tree.put("Game/sound/Zulu.wav", wav); tree.put("Addon/sound/Alpha.mp3", "Apex-authored raw placeholder");
    tree.put("Game/scripts/game_sounds_manifest.txt", R"(// Apex-authored
        game_sounds_manifest { file scripts/events.txt precache_file "scripts/custom/*.txt" preload_file scripts/bad.txt file scripts/missing.txt })");
    tree.put("Addon/scripts/events.txt", R"(// Apex-authored
        "Z.Event" { channel CHAN_AUTO volume "0.5" pitch "90,110" soundlevel SNDLVL_NORM
            wave "*#@><^)(}`~ambient/A.wav"
            rndwave { wave "#b.wav" rndwave { wave "*sound/C.wav" } } }
        "a.Event" { wave "d.wav" }
        "Empty.Event" { channel CHAN_AUTO })");
    tree.put("Game/scripts/custom/extra.txt", "// Apex-authored\nExtra.Event { wave extra.wav }");
    tree.put("Game/scripts/bad.txt", "// Apex-authored\nbroken {"); addon(tree);
    JobPool jobs(2); GameProfile profile; profile.gameDirectory = tree.root / "Game";
    auto vfs = Vfs::load(profile, jobs); REQUIRE(vfs);
    SoundIndex index(*vfs, jobs); finish(jobs, index); auto snapshot = index.snapshot();
    REQUIRE(snapshot->entries.size() == 8);
    REQUIRE(snapshot->find("Extra.Event"));
    CHECK(snapshot->find("Extra.Event")->waves == std::vector<std::string>{"sound/extra.wav"});
    auto entry = snapshot->find("z.event"); REQUIRE(entry);
    CHECK(entry->type == SoundIndexEntry::Type::SoundScript);
    CHECK(entry->waves == std::vector<std::string>{"sound/ambient/a.wav", "sound/b.wav", "sound/c.wav"});
    CHECK(entry->channel == "CHAN_AUTO"); CHECK(entry->volume == "0.5"); CHECK(entry->pitch == "90,110");
    CHECK(entry->soundlevel == "SNDLVL_NORM"); CHECK(entry->mount.has_value());
    CHECK(entry->mount == vfs->provider("scripts/events.txt"));
    REQUIRE(snapshot->find("SOUND/ALPHA.MP3"));
    CHECK(snapshot->find("SOUND/ALPHA.MP3")->mount.has_value());
    for (auto name : {"Empty.Event", "scripts/bad.txt", "scripts/missing.txt"}) {
        REQUIRE(snapshot->find(name)); CHECK_FALSE(snapshot->find(name)->error.empty());
    }
    CHECK_FALSE(snapshot->find("missing")); cancelMidWalk<SoundKind>(*vfs, jobs);
}
TEST_CASE("particle index manifest definitions only provider errors and cancellation") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("Game/particles/particles_manifest.txt", R"(// Apex-authored
        particles_manifest { file "!particles/test.pcf" file particles/bad.pcf file particles/missing.pcf })");
    tree.put("Addon/particles/test.pcf", particleFile());
    tree.put("Game/particles/bad.pcf", "Apex-authored invalid DMX"); addon(tree);
    JobPool jobs(2); GameProfile profile; profile.gameDirectory = tree.root / "Game";
    auto vfs = Vfs::load(profile, jobs); REQUIRE(vfs);
    ParticleIndex index(*vfs, jobs); finish(jobs, index); auto snapshot = index.snapshot();
    REQUIRE(snapshot->entries.size() == 4);
    auto entry = snapshot->find("ALPHA"); REQUIRE(entry); CHECK(entry->name == "alpha");
    CHECK(entry->pcf == "particles/test.pcf"); CHECK(entry->error.empty());
    CHECK(entry->mount == vfs->provider(entry->pcf)); CHECK(entry->mount.has_value());
    REQUIRE(snapshot->find("zulu")); CHECK_FALSE(snapshot->find("NotASystem"));
    for (auto name : {"particles/bad.pcf", "particles/missing.pcf"}) {
        REQUIRE(snapshot->find(name)); CHECK_FALSE(snapshot->find(name)->error.empty());
    }
    cancelMidWalk<ParticleKind>(*vfs, jobs);
}
TEST_CASE("asset index filters names sources used in map and kind fields") {
    ModelIndexEntry modelA, modelB;
    modelA.name = "models/Alpha.mdl"; modelA.staticProp = true; modelA.mount = 1;
    modelB.name = "models/Zulu.mdl";
    ModelIndexSnapshot models({modelB, modelA});
    ModelFilter m;
    CHECK(filterModels(models, m).size() == 2);
    m.name = "ALPHA"; CHECK(filterModels(models, m) == std::vector<std::size_t>{0});
    m.name = "models/?ulu.*"; CHECK(filterModels(models, m) == std::vector<std::size_t>{1});
    m.name = "Zulu*"; CHECK(filterModels(models, m).empty()); m.name.clear();
    m.staticPropOnly = true; CHECK(filterModels(models, m) == std::vector<std::size_t>{0}); m.staticPropOnly = false;
    m.source.emplace(); CHECK(filterModels(models, m) == std::vector<std::size_t>{1});
    m.source.emplace(1); CHECK(filterModels(models, m) == std::vector<std::size_t>{0});
    m.source.emplace(7); CHECK(filterModels(models, m).empty()); m.source.reset();
    m.usedInMap = {"MODELS\\ALPHA.MDL"}; CHECK(filterModels(models, m) == std::vector<std::size_t>{0});
    m.usedInMap.emplace(); CHECK(filterModels(models, m).empty());
    SoundIndexEntry soundA, soundB;
    soundA.name = "Alpha.Event"; soundA.type = SoundIndexEntry::Type::SoundScript; soundA.mount = 0;
    soundB.name = "sound/zulu.wav";
    SoundIndexSnapshot sounds({soundB, soundA}); SoundFilter s;
    CHECK(filterSounds(sounds, s).size() == 2);
    s.name = "ALPHA"; CHECK(filterSounds(sounds, s) == std::vector<std::size_t>{0});
    s.name = "sound/*.wav"; CHECK(filterSounds(sounds, s) == std::vector<std::size_t>{1}); s.name.clear();
    s.type = SoundIndexEntry::Type::Raw; CHECK(filterSounds(sounds, s) == std::vector<std::size_t>{1});
    s.type = SoundIndexEntry::Type::SoundScript; CHECK(filterSounds(sounds, s) == std::vector<std::size_t>{0}); s.type.reset();
    s.source.emplace(); CHECK(filterSounds(sounds, s) == std::vector<std::size_t>{1});
    s.source.emplace(0); CHECK(filterSounds(sounds, s) == std::vector<std::size_t>{0}); s.source.reset();
    s.usedInMap = {"ALPHA.EVENT"}; CHECK(filterSounds(sounds, s) == std::vector<std::size_t>{0});
    s.usedInMap.emplace(); CHECK(filterSounds(sounds, s).empty());
    ParticleIndexEntry particleA, particleB;
    particleA.name = "Alpha"; particleA.mount = 2; particleB.name = "Zulu";
    ParticleIndexSnapshot particles({particleB, particleA}); ParticleFilter p;
    CHECK(filterParticles(particles, p).size() == 2);
    p.name = "LPH"; CHECK(filterParticles(particles, p) == std::vector<std::size_t>{0});
    p.name = "?ulu"; CHECK(filterParticles(particles, p) == std::vector<std::size_t>{1}); p.name.clear();
    p.source.emplace(); CHECK(filterParticles(particles, p) == std::vector<std::size_t>{1});
    p.source.emplace(2); CHECK(filterParticles(particles, p) == std::vector<std::size_t>{0}); p.source.reset();
    p.usedInMap = {"ALPHA"}; CHECK(filterParticles(particles, p) == std::vector<std::size_t>{0});
    p.usedInMap.emplace(); CHECK(filterParticles(particles, p).empty());
}
TEST_CASE("asset indexes finish empty and record corrupt manifests") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    JobPool jobs(2); GameProfile profile; profile.gameDirectory = tree.root / "Game";
    auto vfs = Vfs::load(profile, jobs); REQUIRE(vfs);
    { ModelIndex index(*vfs, jobs); finish(jobs, index); CHECK(index.snapshot()->entries.empty()); }
    { SoundIndex index(*vfs, jobs); finish(jobs, index); CHECK(index.snapshot()->entries.empty()); }
    { ParticleIndex index(*vfs, jobs); finish(jobs, index); CHECK(index.snapshot()->entries.empty()); }
    tree.put("Game/scripts/game_sounds_manifest.txt", "// Apex-authored\nbad {");
    tree.put("Game/particles/particles_manifest.txt", "// Apex-authored\nbad {");
    REQUIRE(vfs->refresh(jobs));
    { SoundIndex index(*vfs, jobs); finish(jobs, index);
      REQUIRE(index.snapshot()->entries.size() == 1); CHECK_FALSE(index.snapshot()->entries[0].error.empty()); }
    { ParticleIndex index(*vfs, jobs); finish(jobs, index);
      REQUIRE(index.snapshot()->entries.size() == 1); CHECK_FALSE(index.snapshot()->entries[0].error.empty()); }
}

TEST_CASE("corrupt model records an error and raw sound walk keeps its provider") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    tree.put("Game/models/gone.mdl", modelHeader());
    tree.put("Game/sound/gone.wav", "Apex-authored");
    JobPool jobs(1); GameProfile profile; profile.gameDirectory = tree.root / "Game";
    auto vfs = Vfs::load(profile, jobs); REQUIRE(vfs);
    std::filesystem::remove(tree.root / "Game/models/gone.mdl");
    ModelIndex models(*vfs, jobs); SoundIndex sounds(*vfs, jobs);
    finish(jobs, models); finish(jobs, sounds);
    REQUIRE(models.snapshot()->entries.size() == 1);
    REQUIRE(sounds.snapshot()->entries.size() == 1);
    CHECK_FALSE(models.snapshot()->entries[0].error.empty());
    REQUIRE(sounds.snapshot()->find("sound/gone.wav"));
    CHECK(sounds.snapshot()->find("sound/gone.wav")->mount == vfs->provider("sound/gone.wav"));
    CHECK(sounds.snapshot()->find("sound/gone.wav")->error.empty());
}

TEST_CASE("model index rejects bad headers and progresses across parse batches") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", "GameInfo { FileSystem { SearchPaths { game |gameinfo_path|. } } }");
    for (int i = 0; i < 70; ++i) tree.put("Game/models/prop" + std::to_string(i) + ".mdl", modelHeader());
    auto bad = modelHeader(); bad[0] = 'X'; tree.put("Game/models/magic.mdl", bad);
    bad = modelHeader(); word(bad, 4, 99); tree.put("Game/models/version.mdl", bad);
    bad = modelHeader(); word(bad, 232, 0xffffffff); tree.put("Game/models/count.mdl", bad);
    JobPool jobs(2); GameProfile profile; profile.gameDirectory = tree.root / "Game";
    auto vfs = Vfs::load(profile, jobs); REQUIRE(vfs);
    auto prefix = vfs->openPrefix("models/prop0.mdl", 12); REQUIRE(prefix); CHECK(prefix->size() == 12);
    CHECK(vfs->open("models/prop0.mdl")->size() == 408);
    CHECK_FALSE(vfs->openPrefix("../models/prop0.mdl", 12));
    ModelIndex index(*vfs, jobs); finish(jobs, index);
    REQUIRE(index.snapshot()->entries.size() == 73);
    for (auto name : {"magic", "version", "count"}) {
        REQUIRE(index.snapshot()->find(name)); CHECK_FALSE(index.snapshot()->find(name)->error.empty());
    }
    CHECK(index.snapshot()->find("prop69")->error.empty());
}
