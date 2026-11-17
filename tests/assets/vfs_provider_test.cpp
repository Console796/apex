#include "assets/vfs.hpp"
#include "fixture.hpp"
#include "provider_fixture.hpp"

#include <doctest/doctest.h>
#include <future>

using namespace apex;
using namespace apex::assets;

namespace {
struct Tree {
    GameTree tree;
    GameProfile profile;
    JobPool jobs{2};
    Tree() {
        tree.put("Game/gameinfo.txt", R"(GameInfo { FileSystem { SearchPaths {
            game "First" game "|gameinfo_path|." game "Last" } } })");
        tree.put("First/materials/shared.vmt", "first");
        tree.put("Game/materials/shared.vmt", "game");
        tree.put("Game/materials/only_game.vmt", "game only");
        tree.put("Last/materials/shared.vmt", "last");
        tree.put("Last/materials/last_only.vmt", "last only");
        profile.gameDirectory = tree.root / "Game";
    }
    static VfsProviderMount mount(std::shared_ptr<MemoryProvider> provider, VfsPosition::Kind kind,
                                  std::string anchor = {}) {
        return {std::move(provider), {kind, std::move(anchor)}};
    }
    Result<Vfs> load(std::vector<VfsProviderMount> providers) {
        return Vfs::load(profile, jobs, {}, std::move(providers));
    }
};
std::shared_ptr<MemoryProvider> provide(std::string name) {
    auto p = std::make_shared<MemoryProvider>(std::move(name));
    p->contents = {{"materials/shared.vmt", "provider"}, {"Materials/Plugin/Mat.VMT", "plugin mat"}};
    return p;
}
std::size_t indexOf(const std::vector<std::string>& labels, std::string_view needle) {
    for (std::size_t i = 0; i < labels.size(); ++i)
        if (labels[i].find(needle) != std::string::npos) return i;
    return labels.size();
}
} // namespace

TEST_CASE("a provider between profile mounts: earlier mounts win around it") {
    Tree t;
    auto provider = provide("between");
    auto vfs = t.load({Tree::mount(provider, VfsPosition::Kind::Before, "/Last")});
    REQUIRE(vfs);
    const auto labels = vfs->mountLabels();
    const auto first = indexOf(labels, "/First"), game = indexOf(labels, "/Game"),
               plugin = indexOf(labels, "provider:between"), last = indexOf(labels, "/Last");
    CHECK(first < game);
    CHECK(game < plugin);
    CHECK(plugin + 1 == last);
    // First mount wins over everything, and removing it exposes the next layer in order.
    CHECK(asText(*vfs->open("materials/shared.vmt")) == "first");
    CHECK(asText(*vfs->open("materials/only_game.vmt")) == "game only");
    CHECK(asText(*vfs->open("MATERIALS/Plugin/mat.vmt")) == "plugin mat");
    CHECK(asText(*vfs->open("materials/last_only.vmt")) == "last only");
    CHECK(vfs->exists("materials/plugin/mat.vmt"));
    CHECK(vfs->walk("materials", "*.vmt").size() == 4);
    CHECK(vfs->list("materials/plugin", "*.vmt") == std::vector<std::string>{"materials/plugin/mat.vmt"});
    CHECK(vfs->displayPath("materials/plugin/mat.vmt") == "Materials/Plugin/Mat.VMT");
    CHECK(!vfs->provider("materials/plugin/mat.vmt"));
    CHECK(vfs->loosePath("materials/plugin/mat.vmt") == std::nullopt);
    CHECK(vfs->loosePath("materials/last_only.vmt"));
    // Where the provider is ahead of the loose copies, it wins (Last loses to it).
    std::filesystem::remove(t.tree.root / "First/materials/shared.vmt");
    std::filesystem::remove(t.tree.root / "Game/materials/shared.vmt");
    REQUIRE(vfs->refresh(t.jobs));
    CHECK(asText(*vfs->open("materials/shared.vmt")) == "provider");
}

TEST_CASE("provider positions: first, last, after, unmatched anchor, registration order") {
    Tree t;
    auto a = provide("a"), b = provide("b"), c = provide("c"), d = provide("d"), e = provide("e");
    auto vfs = t.load({Tree::mount(a, VfsPosition::Kind::First), Tree::mount(b, VfsPosition::Kind::First),
                       Tree::mount(c, VfsPosition::Kind::After, "/Game"),
                       Tree::mount(d, VfsPosition::Kind::After, "/game"),
                       Tree::mount(e, VfsPosition::Kind::Before, "no-such-mount")});
    REQUIRE(vfs);
    const auto labels = vfs->mountLabels();
    CHECK(indexOf(labels, "provider:a") == 0);
    CHECK(indexOf(labels, "provider:b") == 1);
    CHECK(indexOf(labels, "provider:c") == indexOf(labels, "/Game") + 1);
    CHECK(indexOf(labels, "provider:d") == indexOf(labels, "provider:c") + 1);
    CHECK(labels.back() == "provider:e");
    REQUIRE(vfs->notes().size() == 1);
    CHECK(vfs->notes()[0].find("no mount matches anchor 'no-such-mount'") != std::string::npos);
    CHECK(asText(*vfs->open("materials/shared.vmt")) == "provider"); // a wins
}

TEST_CASE("a throwing or failing provider never breaks the load, the walk or the other mounts") {
    Tree t;
    auto files = provide("badfiles"), refresh = provide("badrefresh"), open = provide("badopen");
    files->throwOnFiles = true;
    refresh->throwOnRefresh = true;
    open->throwOnOpen = true;
    open->contents = {{"materials/boom.vmt", "x"}};
    auto vfs = t.load({Tree::mount(files, VfsPosition::Kind::Last), Tree::mount(refresh, VfsPosition::Kind::Last),
                       Tree::mount(open, VfsPosition::Kind::Last)});
    REQUIRE(vfs);
    CHECK(vfs->notes().size() == 2);
    CHECK(vfs->exists("materials/boom.vmt"));
    auto bad = vfs->open("materials/boom.vmt");
    CHECK(!bad);
    CHECK(asText(*vfs->open("materials/last_only.vmt")) == "last only");
    CHECK(vfs->walk("materials", "*.vmt").size() == 4); // 3 loose + boom
    // A provider that cannot list contributes nothing but the others are intact.
    CHECK(!vfs->exists("materials/plugin/mat.vmt"));
}

TEST_CASE("compileGameInfo leaves provider mounts out and says which") {
    Tree t;
    auto vfs = t.load({Tree::mount(provide("hidden"), VfsPosition::Kind::First)});
    REQUIRE(vfs);
    CHECK(vfs->omittedFromCompile() == std::vector<std::string>{"hidden"});
    auto info = vfs->compileGameInfo();
    REQUIRE(info);
    CHECK(info->find("hidden") == std::string::npos);
    CHECK(info->find("provider:") == std::string::npos);
    CHECK(info->find("/First") != std::string::npos);
    auto plain = t.load({});
    REQUIRE(plain);
    CHECK(plain->omittedFromCompile().empty());
    CHECK(*plain->compileGameInfo() == *info);
}

TEST_CASE("walkTask over a provider mount runs on workers and survives refresh of the provider") {
    Tree t;
    auto provider = provide("w");
    for (int i = 0; i < 500; ++i) provider->contents["materials/bulk/m" + std::to_string(i) + ".vmt"] = "m";
    auto vfs = t.load({Tree::mount(provider, VfsPosition::Kind::Last)});
    REQUIRE(vfs);
    CHECK(provider->refreshes == 1);
    std::vector<std::future<std::size_t>> walks;
    for (int i = 0; i < 4; ++i) {
        auto task = vfs->walkTask("materials", "*.vmt");
        walks.push_back(std::async(std::launch::async, [task] { return task().size(); }));
    }
    for (auto& walk : walks) CHECK(walk.get() == 504);
    CHECK(provider->opens == 0); // walking reads the cached listing only
    provider->contents["materials/bulk/late.vmt"] = "m";
    CHECK(vfs->walk("materials", "*.vmt").size() == 504);
    REQUIRE(vfs->refresh(t.jobs));
    CHECK(provider->refreshes == 2);
    CHECK(vfs->walk("materials", "*.vmt").size() == 505);
}
