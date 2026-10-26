#include "fixtures.hpp"
#include "editor/editor_session.hpp"
#include "editor/instance_expansion.hpp"

#include <chrono>
#include <fstream>

using namespace editor_test;
namespace {
struct Tree {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("apex-expansion-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Tree() { std::filesystem::remove_all(root); }
    void put(std::string_view name, std::string_view bytes) {
        std::filesystem::create_directories(root);
        std::ofstream(root / name) << bytes;
    }
};
std::string value(const Document& doc, std::string_view classname, std::string_view key) {
    for (auto h : nodes(doc)) {
        const auto& block = *doc.get(h)->block;
        const auto* cls = block.find("classname");
        const auto* v = block.find(key);
        if (cls && v && cls->decodedValue().value_or("") == classname) return v->decodedValue().value_or("");
    }
    return {};
}
}
TEST_CASE("Instance expansion composes transforms and parent variables on immutable copies") {
    Tree tree;
    tree.put("outer.vmf", R"(entity { classname func_instance targetname inner file leaf.vmf
        origin "10 0 0" angles "19 23 37" replace01 "$skin $parent" replace02 "$mat $material" })");
    tree.put("leaf.vmf", R"(entity { classname prop_static model "$model" skin "$skin" body "$skin" }
        world { classname worldspawn solid { side { material "$mat" } } })");
    Document doc(parsed(R"(entity { classname func_instance targetname outer file outer.vmf origin "100 200 300"
        angles "31 47 13" replace01 "$parent 2" replace02 "$material @test/red" })"));
    const auto before = doc.write().value();
    InstanceTemplates templates;
    InstanceExpansion expansion(templates);
    expansion.update(doc, tree.root / "parent.vmf");
    REQUIRE(expansion.problems().empty());
    REQUIRE(expansion.contents().size() == 2);
    const auto& outer = expansion.contents()[0];
    const auto& inner = expansion.contents()[1];
    CHECK(outer.templ != outer.keys);
    CHECK(inner.depth == 2);
    CHECK(inner.instance == outer.instance);
    CHECK(value(*inner.keys, "prop_static", "skin") == "2");
    CHECK(value(*inner.keys, "prop_static", "body") == "2");
    CHECK(value(*inner.templ, "prop_static", "skin") == "$skin");
    auto parent = block(R"(entity { origin "100 200 300" angles "31 47 13" })");
    auto nested = block(R"(entity { origin "10 0 0" angles "19 23 37" })");
    CHECK(inner.transform == document::entityTransform(document::entityPose(parent)) * document::entityTransform(document::entityPose(nested)));
    const auto leafBefore = inner.templ->write().value();
    const auto copy = inner.keys;
    expansion.update(doc, tree.root / "parent.vmf");
    CHECK(expansion.contents()[1].keys == copy);
    CHECK(expansion.contents()[1].templ->write().value() == leafBefore);
    CHECK(doc.write().value() == before);
    bool material = false;
    for (auto h : nodes(*copy)) if (std::holds_alternative<Handle<Solid>>(h)) {
        const auto* side = copy->get(h)->block->find("side");
        REQUIRE(side);
        CHECK(side->find("material")->decodedValue().value() == "@test/red");
        material = true;
    }
    CHECK(material);
}
TEST_CASE("Instance expansion reports cycles missing files and the depth limit") {
    Tree tree;
    tree.put("a.vmf", "entity { classname func_instance file b.vmf }");
    tree.put("b.vmf", "entity { classname func_instance file a.vmf }");
    Document doc(parsed("entity { classname func_instance file a.vmf } entity { classname func_instance file missing.vmf }"));
    InstanceTemplates templates;
    InstanceExpansion expansion(templates);
    expansion.update(doc, tree.root / "parent.vmf");
    CHECK(expansion.contents().size() == 2);
    REQUIRE(expansion.problems().size() == 2);
    CHECK(expansion.problems()[0].kind == InstanceProblem::Kind::Cycle);
    CHECK(expansion.problems()[1].kind == InstanceProblem::Kind::Unresolved);
    CHECK(expansion.problems()[1].file == "missing.vmf");
    for (int i = 0; i < 10; ++i)
        tree.put(std::to_string(i) + ".vmf", "entity { classname func_instance file " + std::to_string(i + 1) + ".vmf }");
    Document deep(parsed("entity { classname func_instance file 0.vmf }"));
    expansion.update(deep, tree.root / "parent.vmf");
    CHECK(expansion.contents().size() == InstanceTemplates::depthLimit);
    REQUIRE(expansion.problems().size() == 1);
    CHECK(expansion.problems()[0].kind == InstanceProblem::Kind::DepthLimit);
}
TEST_CASE("Instance reload replaces only users and preserves a good template on failure") {
    Tree tree;
    tree.put("a.vmf", "entity { classname info_target targetname old }");
    tree.put("b.vmf", "entity { classname info_target targetname other }");
    Document doc(parsed("entity { classname func_instance file a.vmf } entity { classname func_instance file b.vmf }"));
    InstanceTemplates templates;
    InstanceExpansion expansion(templates);
    expansion.update(doc, tree.root / "parent.vmf");
    const auto a = expansion.contents()[0], b = expansion.contents()[1];
    tree.put("a.vmf", "entity { classname info_target targetname new }");
    auto reload = expansion.invalidate(tree.root / "a.vmf");
    CHECK(reload.users == std::vector<NodeId>{a.instance});
    CHECK_FALSE(reload.problem);
    expansion.update(doc, tree.root / "parent.vmf");
    CHECK(expansion.contents()[0].templ != a.templ);
    CHECK(expansion.contents()[1].keys == b.keys);
    const auto good = expansion.contents()[0];
    tree.put("a.vmf", "entity {");
    reload = expansion.invalidate(tree.root / "a.vmf");
    REQUIRE(reload.problem);
    expansion.update(doc, tree.root / "parent.vmf");
    CHECK(expansion.contents()[0].keys == good.keys);
    REQUIRE(expansion.problems().size() == 1);
    CHECK(expansion.problems()[0].kind == InstanceProblem::Kind::Reload);
}
TEST_CASE("Instance contents pick their owner and visibility persists outside undo") {
    Tree tree;
    Document templ(parsed());
    addBox(templ, {96, -8, -8}, {112, 8, 8});
    tree.put("a.vmf", templ.write().value());
    Document doc(parsed("viewsettings { nInstanceVisibility 2 other untouched } entity { classname func_instance file a.vmf origin \"10 0 0\" }"));
    EditorSession session;
    session.attach(doc);
    session.setConnectionContext(tree.root / "parent.vmf");
    const auto owner = session.instances().contents().front().instance;
    const Ray ray{{110, 0, 100}, {0, 0, -1}};
    auto hit = session.picks().first(ray);
    REQUIRE(hit);
    CHECK(hit->node == owner);
    CHECK_FALSE(hit->side);
    session.pointerClick(ray);
    CHECK(session.selection().ordered().size() == 1);
    CHECK(session.copyText().value().find("solid") == std::string::npos);
    const std::array<Plane, 6> planes{{{{1,0,0}, 130}, {{-1,0,0}, -90}, {{0,1,0}, 20}, {{0,-1,0}, 20}, {{0,0,1}, 20}, {{0,0,-1}, 20}}};
    CHECK(session.picks().frustum(planes, Mode::Touching) == std::vector<NodeId>{owner});
    session.setInstanceVisibility(InstanceVisibility::Hidden);
    CHECK_FALSE(session.picks().first(ray));
    CHECK(session.picks().first({{10,0,100}, {0,0,-1}})->node == owner);
    CHECK(doc.undoCount() == 0);
    CHECK(doc.toVmf()->viewSettings->instanceVisibility()->asInt().value() == 0);
    session.setInstanceVisibility(InstanceVisibility::Tinted);
    CHECK(session.picks().first(ray)->node == owner);
    CHECK(doc.undoCount() == 0);
    CHECK(doc.write().value().find("untouched") != std::string::npos);
    Document reopened(doc.toVmf().value());
    EditorSession other;
    other.attach(reopened);
    CHECK(other.instanceVisibility() == InstanceVisibility::Tinted);
}
