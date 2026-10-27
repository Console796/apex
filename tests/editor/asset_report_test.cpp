#include "fixtures.hpp"
#include "editor/asset_report.hpp"

using namespace editor_test;

TEST_CASE("Asset report groups missing assets by kind and preserves references and addon hints") {
    Document doc(parsed(R"(
world { id 1 classname worldspawn solid { id 2
 side { id 3 material "Addon/Wall" }
 side { id 4 material "addon/wall" }
} }
entity { id 5 classname prop_dynamic model "Models/Test.mdl" studio "models/test.mdl" }
entity { id 6 classname ambient_generic sound "Test.Sound" message "not_a_sound" }
entity { id 7 classname info_particle_system effect_name "test_particle" }
entity { id 8 classname func_instance file "maps/test.vmf" }
entity { id 9 classname env_sprite texture "addon/wall" }
)"));
    const auto original = doc.write().value();
    const auto hint = [](AssetKind kind, std::string_view path) -> std::optional<std::string> {
        if (kind == AssetKind::Material && path == "addon/wall") return "Fixture addon";
        return {};
    };
    AssetReport report(doc, [](auto, auto) { return false; }, hint, hint);
    REQUIRE(report.missing().size() == 5);
    const auto& material = report.missing()[0];
    CHECK(material.kind == AssetKind::Material);
    CHECK(material.path == "Addon/Wall");
    CHECK(material.normalizedPath == "addon/wall");
    CHECK(material.addon == "Fixture addon");
    REQUIRE(material.references.size() == 3);
    CHECK(material.references[0].solid != 0);
    CHECK(material.references[0].side == 3);
    CHECK(material.references[1].side == 4);
    CHECK(material.references[2].entity != 0);
    CHECK(material.references[2].solid == 0);
    CHECK(report.missing()[1].kind == AssetKind::Model);
    CHECK(report.missing()[1].references.size() == 2);
    CHECK(report.missing()[2].kind == AssetKind::Sound);
    CHECK(report.missing()[3].kind == AssetKind::Particle);
    CHECK(report.missing()[4].kind == AssetKind::Instance);
    CHECK(report.addonsUsed() == std::set<std::string>{"Fixture addon"});
    AssetReport mounted(doc, [](auto, auto) { return true; }, {}, hint);
    CHECK(mounted.missing().empty());
    CHECK(mounted.addonsUsed() == report.addonsUsed());
    CHECK(doc.write().value() == original);
    CHECK(doc.undoCount() == 0);
}

TEST_CASE("Asset report Problems combines unknown classes invalid connections and missing assets") {
    Document doc(parsed(R"(entity { classname mystery model "models/missing.mdl"
 connections { OnTrigger "missing,Open,,0,-1" }
})"));
    assets::EntityClassTable classes;
    NameIndex names(doc);
    ConnectionValidator validator(doc, names, classes);
    AssetReport report(doc, [](auto, auto) { return false; });
    const auto problems = report.problems(doc, validator, [&](auto name) { return classes.find(name) != nullptr; });
    CHECK(problems.missingAssets.size() == 1);
    REQUIRE(problems.invalidConnections.size() == 1);
    CHECK(problems.invalidConnections[0].entity == doc.roots()[0]);
    CHECK(problems.invalidConnections[0].output == "OnTrigger");
    CHECK_FALSE(problems.invalidConnections[0].issues.empty());
    REQUIRE(problems.unknownClasses.size() == 1);
    CHECK(problems.unknownClasses[0].classname == "mystery");
}

TEST_CASE("Asset report uses FGD key types and ignores untyped message and brush model references") {
    Document doc(parsed(R"(entity { classname fixture message "Fixture.Sound" custommat "Fancy/Wall" model "*12" }
entity { classname unknown message "prose" }
entity { classname func_instance file "maps/fixture.vmf" })"));
    assets::EntityClass cls;
    cls.name = "fixture";
    assets::EntityProperty sound, material;
    sound.name = "message"; sound.type = "sound";
    material.name = "custommat"; material.type = "material";
    cls.properties = {sound, material};
    assets::EntityClassTable classes;
    classes.classes.emplace("fixture", cls);
    assets::EntityClass instance;
    instance.name = "func_instance";
    assets::EntityProperty file;
    file.name = "file"; file.type = "string";
    instance.properties.push_back(file);
    classes.classes.emplace("func_instance", instance);
    AssetReport report(doc, [](auto, auto) { return false; }, {}, {}, &classes);
    REQUIRE(report.missing().size() == 3);
    CHECK(report.missing()[0].kind == AssetKind::Material);
    CHECK(report.missing()[1].kind == AssetKind::Sound);
    CHECK(report.missing()[2].kind == AssetKind::Instance);
}
