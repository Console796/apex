#include "assets/fgd.hpp"
#include "core/entity_helpers.hpp"
#include "fixture.hpp"

#include <cstdlib>
#include <set>
#include <sstream>
#include <doctest/doctest.h>

using namespace apex::assets;

TEST_CASE("FGD resolves inherited drawing properties and copies string storage") {
    GameTree tree;
    tree.put("base.fgd", R"(@BaseClass color(12 34 56) size(-1 -2 -3, 4 5 6) iconsprite("editor/base.vmt") = Drawing []
        @BaseClass base(Drawing) = Middle []
    )");
    tree.put("game.fgd", R"(@include "base.fgd"
        @PointClass base(Middle) color(1 2 3) = example : "Example" []
        @SolidClass base(Drawing) = brush []
        @PointClass size(8 16 32) = centered []
    )");
    auto table = loadEntityClasses(tree.root / "game.fgd");
    REQUIRE(table);
    CHECK(table->unresolvedBases.empty());
    auto entity = table->find("EXAMPLE");
    REQUIRE(entity);
    CHECK(entity->type == "PointClass");
    CHECK(entity->color == std::array<int, 3>{1, 2, 3});
    CHECK(entity->mins == std::array<double, 3>{-1, -2, -3});
    CHECK(entity->maxs == std::array<double, 3>{4, 5, 6});
    CHECK(entity->iconSprite == "editor/base.vmt");
    REQUIRE(table->find("brush"));
    CHECK(table->find("brush")->type == "SolidClass");
    REQUIRE(table->find("centered"));
    CHECK(table->find("centered")->mins == std::array<double, 3>{-4, -8, -16});
    CHECK_FALSE(table->find("missing"));
}

TEST_CASE("FGD diagnoses unresolved bases missing files and inheritance cycles") {
    GameTree tree;
    tree.put("missing.fgd", "@PointClass base(Absent) = entity []\n");
    auto missing = loadEntityClasses(tree.root / "missing.fgd");
    REQUIRE(missing);
    CHECK(missing->unresolvedBases == std::vector<std::string>{"entity: Absent"});
    CHECK_FALSE(loadEntityClasses(tree.root / "absent.fgd"));
    tree.put("cycle.fgd", "@BaseClass base(B) = A []\n@BaseClass base(A) = B []\n");
    CHECK_FALSE(loadEntityClasses(tree.root / "cycle.fgd"));
}

TEST_CASE("FGD real P2CE entity table has no unresolved bases") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    auto table = loadEntityClasses(std::filesystem::path(install) / "p2ce/p2ce.fgd");
    REQUIRE_MESSAGE(table, (table ? "" : table.error().what()));
    MESSAGE(table->classes.size(), " FGD classes, ", table->unresolvedBases.size(), " unresolved bases");
    CHECK(table->classes.size() > 100);
    CHECK(table->find("npc_headcrab_black")); // Last definition: guard against a partial parse.
    for (const auto& base : table->unresolvedBases) { INFO(base); CHECK(false); }
    REQUIRE(table->find("info_player_start"));
}

TEST_CASE("FGD property order spans kinds includes bases and overrides") {
    GameTree tree;
    tree.put("base.fgd", R"(@BaseClass = First [
        z(string) : "Z" : "base" : "Description"
        mode(choices) : "Mode" : 2 = [ 1 : "One" 2 : "Two" ]
        bits(flags) = [ 1 : "Enabled" : 1 4 : "Other" : 0 ]
        input Enable(void) : "Enable it"
        output OnDone(void) : "Done"
    ]
    @BaseClass = Second [ z(string) : "Other" : "second" a(integer) : "A" : 3 ]
    )");
    tree.put("main.fgd", R"(@include "base.fgd"
        @PointClass base(First, Second) bbox(-2 -3 -4, 5 6 7) = test [
            z(string) readonly : "Override" : "direct"
            last(float) : "Last" : "1.5" : "Contains fake(string), [ and //"
        ]
        @ExtendClass base(First, Second) bbox(-2 -3 -4, 5 6 7) = test [ extra(string) : "Extra" : "" ]
        @PointClass base(First, Second) = inherited []
    )");
    const auto table = loadEntityClasses(tree.root / "main.fgd");
    REQUIRE_MESSAGE(table, (table ? "" : table.error().what()));
    const auto* cls = table->find("test");
    REQUIRE(cls);
    std::vector<std::string> names;
    for (const auto& prop : cls->properties) names.push_back(prop.name);
    REQUIRE(names == std::vector<std::string>{"z", "mode", "bits", "a", "last", "extra"});
    REQUIRE(cls->properties[1].choices.size() == 2);
    REQUIRE(cls->properties[2].flags.size() == 2);
    CHECK(cls->properties[0].defaultValue == "direct");
    CHECK(cls->properties[0].displayName == "Override");
    CHECK(cls->properties[0].readOnly);
    CHECK(cls->properties[1].choices[1].value == "2");
    CHECK(cls->properties[1].choices[1].label == "Two");
    CHECK(cls->properties[2].flags[0].bit == 1);
    CHECK(cls->properties[2].flags[0].defaultValue);
    CHECK(cls->properties[2].defaultValue == "1");
    REQUIRE(cls->inputs.size() == 1);
    CHECK(cls->inputs[0].name == "Enable");
    REQUIRE(cls->outputs.size() == 1);
    CHECK(cls->outputs[0].description == "Done");
    CHECK(cls->mins == std::array<double, 3>{-2, -3, -4});
    CHECK(cls->maxs == std::array<double, 3>{5, 6, 7});
    CHECK(table->find("Second")->properties[0].defaultValue == "second");
    REQUIRE(table->find("inherited"));
    CHECK(table->find("inherited")->properties[0].defaultValue == "base");
}

TEST_CASE("FGD helpers preserve raw arguments declaration order and all inherited helpers") {
    GameTree tree;
    tree.put("base.fgd", R"fgd(@BaseClass sphere(firstRadius) color(12 34 56) = First []
        @BaseClass studio("models/base.mdl") = Second []
        @BaseClass base(fIrSt, SECOND) sphere(middleRadius) = Middle []
    )fgd");
    tree.put("helpers.fgd", R"fgd(@include "base.fgd"
        @PointClass base(MIDDLE, second)
            Studio("models/a,b(c).mdl") Foo(a, (b, c))
            sphere(ownRadius) iconsprite("editor/a,b(c).vmt")
            size(8 16 32) bbox(-1 -2 -3, 4 5 6) color(1 2 3)
            = Example []
        @PointClass sprite() = after []
    )fgd");
    auto table = loadEntityClasses(tree.root / "helpers.fgd");
    REQUIRE_MESSAGE(table, (table ? "" : table.error().what()));
    const auto* entity = table->find("EXAMPLE");
    REQUIRE(entity);
    std::vector<std::string> names;
    for (const auto& h : entity->helpers) names.push_back(h.name);
    REQUIRE(names == std::vector<std::string>{"sphere", "color", "studio", "sphere", "studio",
        "studio", "foo", "sphere", "iconsprite", "size", "bbox", "color"});
    CHECK(entity->helpers[0].arguments == "firstRadius");
    CHECK(entity->helpers[3].arguments == "middleRadius");
    CHECK(entity->helpers[5].arguments == R"("models/a,b(c).mdl")");
    CHECK(entity->helpers[6].arguments == "a, (b, c)");
    CHECK(splitHelperArguments(entity->helpers[6].arguments) == std::vector<std::string>{"a", "(b, c)"});
    CHECK(std::get<StudioHelper>(parseHelper(entity->helpers[5])).model == "models/a,b(c).mdl");
    CHECK(std::get<UnknownHelper>(parseHelper(entity->helpers[6])).arguments == "a, (b, c)");
    CHECK(entity->iconSprite == "editor/a,b(c).vmt");
    CHECK(entity->color == std::array<int, 3>{1, 2, 3});
    CHECK(entity->mins == std::array<double, 3>{-1, -2, -3});
    CHECK(entity->maxs == std::array<double, 3>{4, 5, 6});
    REQUIRE(table->find("after"));
    REQUIRE(table->find("after")->helpers.size() == 1);
    CHECK(table->find("after")->helpers[0].arguments.empty());
    REQUIRE(table->find("first"));
    CHECK(table->find("first")->helpers.size() == 2);
}

TEST_CASE("FGD helper corpus parses all supported kinds and reports unknown names") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    auto table = loadEntityClasses(std::filesystem::path(install) / "p2ce/p2ce.fgd");
    REQUIRE_MESSAGE(table, (table ? "" : table.error().what()));
    const std::set<std::string> known{"sphere", "line", "iconsprite", "studio", "studioprop",
        "size", "frustum", "animator", "keyframe", "cylinder", "sidelist", "lightprop", "sprite",
        "wirebox", "lightcone", "worldtext", "decal", "orientedwidthheight", "orientedwidthheighthalf",
        "bbox", "clusteredlight", "overlay_transition", "instance", "sweptplayerhull", "overlay",
        "orientedboundingbox", "light", "quadbounds", "catapult", "color"};
    std::set<std::string> unknown;
    std::map<std::string, std::size_t> counts;
    for (const auto& [name, entity] : table->classes) {
        for (const auto& h : entity.helpers) {
            INFO(name, ": ", h.name, "(", h.arguments, ")");
            const auto typed = parseHelper(h);
            const auto* other = std::get_if<UnknownHelper>(&typed);
            CHECK((other == nullptr) == known.contains(h.name));
            if (other) {
                unknown.insert(other->name);
                CHECK(other->arguments == h.arguments);
            }
            ++counts[h.name];
        }
    }
    CHECK(counts["sphere"] > 0);
    CHECK(counts["line"] > 0);
    CHECK(counts["studio"] > 0);
    std::ostringstream names;
    for (const auto& name : unknown) names << name << ' ';
    MESSAGE("Unknown FGD helpers: {", names.str(), "}");
    MESSAGE("Inherited helper counts: sphere=", counts["sphere"], ", line=", counts["line"],
        ", studio=", counts["studio"]);
}
