#include "assets/material_index.hpp"
#include "core/json.hpp"
#include "core/strings.hpp"
#include "fixture.hpp"

#include <doctest/doctest.h>
#include <thread>
#include <vpkpp/format/VPK.h>

using namespace apex;
using namespace apex::assets;
namespace {
void finish(JobPool& jobs, MaterialIndex& index) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!index.progress().finished && std::chrono::steady_clock::now() < deadline) {
        index.step();
        jobs.drainCompletions();
        std::this_thread::yield();
    }
    REQUIRE(index.progress().finished);
}
}
TEST_CASE("material index walks loose VPK and addons and resolves patches and directives") {
    GameTree tree;
    tree.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths {
        game |gameinfo_path|. game Pak/pak.vpk
    } } })");
    tree.put("Game/Materials/Nested/Base.VMT", R"(LightmappedGeneric {
        $basetexture base %ToolViewportTexture viewport %tooltexture tool
        %keywords "Metal, Factory" $keywords "Rusty panel"
    })");
    tree.put("Game/materials/patch.vmt", R"(patch { include materials/nested/base.vmt
        replace { %ToolViewportTexture patched } })");
    tree.put("Game/materials/cycle.vmt", "patch { include cycle }");
    tree.put("Game/materials/bad.vmt", "UnlitGeneric {");
    tree.put("Game/materials/tools/empty.vmt", "UnlitGeneric { }");
    tree.put("Game/materials/explicit.vmt", "UnlitGeneric { %tooltexture \"\" }");
    tree.put("Game/materials/directive.vmt", "#include materials/shader.inc");
    tree.put("Game/materials/shader.inc", "UnlitGeneric { $basetexture directive }");
    tree.put("Game/materials/shadow.vmt", "UnlitGeneric { $basetexture game }");
    tree.put("Addon/materials/Shadow.VMT", "VertexLitGeneric { $basetexture addon }");
    constexpr auto header = "<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->\n";
    tree.put("Addon/addon.kv3", std::string(header) + "{ mod = \"Index fixture\" }");
    tree.put("Game/addons/addonlist.kv3", std::string(header) + "{ addons = [ { dir = " +
        json::write({(tree.root / "Addon").string()}) + " mounted = true type = \"local\" vpks = [] } ] }");
    std::filesystem::create_directories(tree.root / "Pak");
    auto archive = vpkpp::VPK::create((tree.root / "Pak/pak_dir.vpk").string(), 1);
    REQUIRE(archive);
    constexpr std::string_view text = "UnlitGeneric { $basetexture packed }";
    REQUIRE(archive->addEntry("materials/deep/packed.vmt", std::span(
        reinterpret_cast<const std::byte*>(text.data()), text.size())));
    REQUIRE(archive->bake());
    archive.reset();
    JobPool jobs(2);
    GameProfile profile;
    profile.gameDirectory = tree.root / "Game";
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    MaterialIndex index(*vfs, jobs);
    CHECK_FALSE(index.snapshot());
    finish(jobs, index);
    auto snapshot = index.snapshot();
    REQUIRE(snapshot);
    CHECK(snapshot->entries.size() == 9);
    CHECK(index.progress().done == index.progress().total);
    auto base = snapshot->find("MATERIALS/NESTED/BASE.VMT");
    REQUIRE(base);
    CHECK(base->name == "Nested/Base");
    CHECK(base->shader == "lightmappedgeneric");
    CHECK(base->previewTexture == "viewport");
    CHECK(base->toolTexture);
    CHECK(base->keywords == std::vector<std::string>{"metal", "factory", "rusty", "panel"});
    REQUIRE(snapshot->find("patch"));
    CHECK(snapshot->find("patch")->shader == base->shader);
    CHECK(snapshot->find("patch")->previewTexture == "patched");
    CHECK(snapshot->find("patch")->keywords == base->keywords);
    for (const auto& entry : snapshot->entries) {
        auto material = loadMaterial(*vfs, entry.name);
        if (material) {
            CHECK(entry.shader == toLowerAscii(material->shader));
            CHECK(entry.previewTexture == material->previewTexture());
        } else CHECK_FALSE(entry.error.empty());
    }
    REQUIRE(snapshot->find("shadow"));
    CHECK(snapshot->find("shadow")->previewTexture == "addon");
    CHECK(snapshot->find("shadow")->mount == vfs->provider("materials/shadow.vmt"));
    CHECK(snapshot->find("shadow")->mount.has_value());
    CHECK(snapshot->find("tools/empty")->toolTexture);
    CHECK(snapshot->find("explicit")->toolTexture);
    CHECK(snapshot->find("directive")->previewTexture == "directive");
    CHECK_FALSE(snapshot->find("cycle")->error.empty());
    CHECK_FALSE(snapshot->find("bad")->error.empty());
    CHECK(snapshot->find("missing") == nullptr);
    for (int i = 0; i < 20; ++i) {
        MaterialIndex temporary(*vfs, jobs);
        temporary.step();
        temporary.cancel();
    }
    {
        MaterialIndex temporary(*vfs, jobs);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!temporary.progress().total && std::chrono::steady_clock::now() < deadline) {
            jobs.drainCompletions();
            std::this_thread::yield();
        }
        REQUIRE(temporary.progress().total == 9);
        temporary.step(); // schedules parsing of owned bytes, then cancels mid-build
    }
    // Walk tasks own their backing state, including when Vfs is destroyed first.
    { MaterialIndex temporary(*vfs, jobs); vfs = std::unexpected(Error{ErrorCode::Internal, "released"}); }
    jobs.drainCompletions();
}

TEST_CASE("material index filters substring glob keyword shader tools and used names") {
    MaterialIndexEntry a, b, c;
    a.name = "Brick/Wall"; a.shader = "lightmappedgeneric"; a.keywords = {"rusty", "factory"};
    b.name = "tools/Clip"; b.shader = "unlitgeneric"; b.toolTexture = true;
    c.name = "brick/Floor"; c.shader = a.shader; c.keywords = {"factory"}; c.mount = 1;
    b.mount = 0;
    MaterialIndexSnapshot snapshot({a, b, c});
    auto names = [&](MaterialFilter filter) {
        std::vector<std::string> result;
        for (auto i : filterMaterials(snapshot, filter)) result.push_back(snapshot.entries[i].name);
        return result;
    };
    MaterialFilter f;
    f.name = "BRICK";
    CHECK(names(f) == std::vector<std::string>{"brick/Floor", "Brick/Wall"});
    f.name = "*/?all";
    CHECK(names(f) == std::vector<std::string>{"Brick/Wall"});
    f.name = "Wall*";
    CHECK(names(f).empty()); // glob matches the full name
    f.name = "brick keyword:FACTORY keyword:rusty";
    CHECK(names(f) == std::vector<std::string>{"Brick/Wall"});
    f.name = "keyword:rust";
    CHECK(names(f).empty());
    f.name = ""; f.shader = "UNLITGENERIC";
    CHECK(names(f) == std::vector<std::string>{"tools/Clip"});
    f.toolTextures = false;
    CHECK(names(f).empty());
    f.shader = "";
    CHECK(names(f).size() == 2);
    f.usedInMap = std::set<std::string>{"MATERIALS/BRICK/WALL.VMT", "missing"};
    CHECK(names(f) == std::vector<std::string>{"Brick/Wall"});
    f.usedInMap = std::set<std::string>{};
    CHECK(names(f).empty());
    MaterialFilter s;
    s.source.emplace(); // game content only
    CHECK(names(s) == std::vector<std::string>{"Brick/Wall"});
    s.source.emplace(1);
    CHECK(names(s) == std::vector<std::string>{"brick/Floor"});
    s.source.emplace(0);
    CHECK(names(s) == std::vector<std::string>{"tools/Clip"});
    s.source.emplace(7);
    CHECK(names(s).empty());
}
