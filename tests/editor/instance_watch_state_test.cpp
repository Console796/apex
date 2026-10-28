#include "fixtures.hpp"
#include "editor/instance_expansion.hpp"
#include "editor/instance_watch_state.hpp"

#include <chrono>
#include <fstream>

using namespace editor_test;
using apex::editor::InstanceWatchState;
namespace {
struct Tree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("apex-watch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Tree() { std::filesystem::create_directories(root); }
    ~Tree() { std::filesystem::remove_all(root); }
    std::filesystem::path put(std::string_view name, std::string_view bytes) {
        std::filesystem::create_directories((root / name).parent_path());
        std::ofstream(root / name, std::ios::binary | std::ios::trunc) << bytes;
        return root / name;
    }
};
}

TEST_CASE("Instance watch state ignores directory activity that did not touch a watched file") {
    Tree tree;
    const auto a = std::filesystem::canonical(tree.put("a.vmf", "entity { classname logic_relay }"));
    const auto b = std::filesystem::canonical(tree.put("b.vmf", "entity { classname logic_relay }"));
    InstanceWatchState state;
    const auto watches = state.sync({a, b}, {});
    CHECK(watches.files == std::set<std::filesystem::path>{a, b});
    CHECK(watches.directories == std::set<std::filesystem::path>{a.parent_path()});
    // Something else in the folder, and a notification with nothing new, reload nothing.
    tree.put("neighbour.txt", "x");
    CHECK(state.directoryChanged(a.parent_path()).empty());
    // Only the file that changed is reported, and it is reported once.
    tree.put("a.vmf", "entity { classname logic_relay targetname longer }");
    CHECK(state.directoryChanged(a.parent_path()) == std::set<std::filesystem::path>{a});
    CHECK(state.directoryChanged(a.parent_path()).empty());
    // A deleted dependency counts as changed too.
    std::filesystem::remove(b);
    CHECK(state.directoryChanged(a.parent_path()) == std::set<std::filesystem::path>{b});
    // Another directory's event is not ours.
    CHECK(state.directoryChanged(tree.root / "elsewhere").empty());
}

TEST_CASE("Instance watch state reports a missing template coming back, even under another case or in a new folder") {
    Tree tree;
    const auto sub = tree.root / "sub";
    const auto flat = tree.root / "leaf.vmf";
    const auto deep = sub / "deep.vmf";
    InstanceWatchState state;
    auto watches = state.sync({}, {flat, deep});
    // Missing files are heard from the nearest folder that exists.
    CHECK(watches.directories == std::set<std::filesystem::path>{tree.root});
    CHECK(state.directoryChanged(tree.root).empty());
    tree.put("LEAF.VMF", "entity { classname logic_relay }");
    const auto back = state.directoryChanged(tree.root);
    REQUIRE(back.size() == 1);
    CHECK(back.begin()->filename() == "LEAF.VMF");
    // The folder appears first: re-sync listens inside it, and the file arriving there is reported.
    std::filesystem::create_directories(sub);
    CHECK(state.directoryChanged(tree.root).empty());
    watches = state.sync({}, {flat, deep});
    CHECK(watches.directories.contains(sub));
    tree.put("sub/deep.vmf", "entity { classname logic_relay }");
    CHECK(state.directoryChanged(sub) == std::set<std::filesystem::path>{deep});
}

TEST_CASE("Instance expansion records where unresolved templates would have to appear") {
    Tree tree;
    Document doc(parsed(R"(entity { id 5 classname func_instance targetname i file "gone\\leaf.vmf" })"));
    apex::editor::InstanceTemplates templates;
    apex::editor::InstanceExpansion expansion(templates);
    expansion.update(doc, tree.root / "parent.vmf");
    REQUIRE(expansion.problems().size() == 1);
    CHECK(expansion.dependencies().empty());
    CHECK(expansion.missing() == std::set<std::filesystem::path>{tree.root / "gone" / "leaf.vmf"});
    tree.put("gone/leaf.vmf", "entity { classname logic_relay }");
    expansion.update(doc, tree.root / "parent.vmf");
    CHECK(expansion.problems().empty());
    CHECK(expansion.missing().empty());
}
