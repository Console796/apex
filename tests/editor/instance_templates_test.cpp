#include "fixtures.hpp"
#include "editor/instance_templates.hpp"

#include <chrono>
#include <fstream>

using namespace editor_test;
namespace {
struct Tree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("apex-instance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Tree() { std::filesystem::remove_all(root); }
    void put(const std::filesystem::path& path, std::string_view text = "entity { classname func_instance_io_proxy targetname relay }") {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream(root / path) << text;
    }
};
}
TEST_CASE("Instance resolution searches ordered roots and Source path spelling") {
    Tree tree;
    tree.put("maps/sub/map.vmf", "");
    tree.put("maps/sub/Parts/File.VMF");
    tree.put("maps/Parts/File.VMF");
    tree.put("extra/Parts/File.VMF");
    const auto map = tree.root / "maps/sub/map.vmf";
    const auto extra = tree.root / "extra";
    CHECK(resolveInstanceFile(map, "parts\\file.vmf", extra) == std::filesystem::canonical(tree.root / "maps/sub/Parts/File.VMF"));
    std::filesystem::remove_all(tree.root / "maps/sub/Parts");
    CHECK(resolveInstanceFile(map, "parts/file.vmf", extra) == std::filesystem::canonical(tree.root / "maps/Parts/File.VMF"));
    CHECK(resolveInstanceFile(map, "../Parts/File.VMF") == std::filesystem::canonical(tree.root / "maps/Parts/File.VMF"));
    std::filesystem::remove_all(tree.root / "maps/Parts");
    CHECK(resolveInstanceFile(map, "parts/file.vmf", extra) == std::filesystem::canonical(extra / "Parts/File.VMF"));
    CHECK_FALSE(resolveInstanceFile(map, "missing.vmf", extra));
    CHECK_FALSE(resolveInstanceFile(map, "", extra));
    tree.put("outside/map.vmf", "");
    CHECK(resolveInstanceFile(tree.root / "outside/map.vmf", "parts/file.vmf", extra) ==
        std::filesystem::canonical(extra / "Parts/File.VMF"));
    tree.put("maps/nested/maps/sub/map.vmf", "");
    tree.put("maps/nested/maps/near.vmf");
    tree.put("maps/far.vmf");
    CHECK(resolveInstanceFile(tree.root / "maps/nested/maps/sub/map.vmf", "near.vmf"));
    CHECK_FALSE(resolveInstanceFile(tree.root / "maps/nested/maps/sub/map.vmf", "far.vmf"));
}
TEST_CASE("Instance templates cache immutable documents failures and traversal guards") {
    Tree tree;
    tree.put("template.vmf");
    InstanceTemplates cache;
    const auto path = tree.root / "template.vmf";
    auto first = cache.load(path);
    REQUIRE(first);
    CHECK(proxyNames(**first) == std::vector<std::string>{"relay"});
    CHECK(looksLikeInstanceFile(**first));
    CHECK((*first)->undoCount() == 0);
    tree.put("template.vmf", "invalid {");
    CHECK(cache.load(path).value() == *first);
    std::vector<std::filesystem::path> ancestors{path};
    CHECK_FALSE(cache.load(path, ancestors));
    ancestors.assign(InstanceTemplates::depthLimit, tree.root / "other");
    CHECK_FALSE(cache.load(path, ancestors));
    auto failed = cache.load(tree.root / "missing.vmf");
    CHECK_FALSE(failed);
    tree.put("missing.vmf");
    CHECK_FALSE(cache.load(tree.root / "missing.vmf"));
    CHECK_FALSE(cache.load(tree.root / "broken.vmf"));
    tree.put("broken2.vmf", "entity {");
    CHECK_FALSE(cache.load(tree.root / "broken2.vmf"));
    tree.put("broken2.vmf");
    CHECK_FALSE(cache.load(tree.root / "broken2.vmf"));
    Document unnamed(parsed("entity { classname func_instance_io_proxy }"));
    CHECK(looksLikeInstanceFile(unnamed));
    CHECK(proxyNames(unnamed).empty());
}
