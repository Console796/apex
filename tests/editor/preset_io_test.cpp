#include "editor/pie_menus.hpp"
#include "editor/preset_io.hpp"

#include <doctest/doctest.h>

#include <filesystem>

using namespace apex::editor;

namespace {
KeyChord chord(std::string_view text) { return KeyChord::parse(text).value(); }

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "apex_preset_io_test";
    TempDir() { std::filesystem::remove_all(path); }
    ~TempDir() { std::filesystem::remove_all(path); }
};
}

TEST_CASE("presets round trip through JSON") {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        const auto parsed = parsePreset(serializePreset(*preset));
        REQUIRE(parsed);
        CHECK(parsed->name == preset->name);
        REQUIRE(parsed->bindings.size() == preset->bindings.size());
        for (const auto& [id, binding] : preset->bindings) {
            CAPTURE(id);
            const auto& got = parsed->bindings.at(id);
            CHECK(got.context == binding.context);
            CHECK(got.provisional == binding.provisional);
            CHECK(got.value.index() == binding.value.index());
            if (const auto* keys = std::get_if<std::vector<KeyChord>>(&binding.value))
                CHECK(std::get<std::vector<KeyChord>>(got.value) == *keys);
        }
    }
}

TEST_CASE("parsing tolerates missing and unknown actions but not duplicate chords") {
    auto parsed = parsePreset(R"({"format":1,"name":"Few","bindings":{
        "edit.undo":{"keys":["Ctrl+Z"]},"no.such.action":{"keys":["Q"]}}})");
    REQUIRE(parsed);
    CHECK(parsed->bindings.size() == actionRegistry().entries().size());
    CHECK(std::holds_alternative<Unbound>(parsed->bindings.at("edit.redo").value));
    CHECK_FALSE(parsed->bindings.contains("no.such.action"));
    CHECK_NOTHROW(static_cast<void>(Keymap(*parsed)));

    CHECK_FALSE(parsePreset(R"({"format":1,"name":"Dup","bindings":{
        "edit.undo":{"keys":["Ctrl+Z"]},"edit.redo":{"keys":["Ctrl+Z"]}}})"));
    CHECK_FALSE(parsePreset(R"({"format":2,"name":"X","bindings":{}})"));
    CHECK_FALSE(parsePreset("not json"));
    CHECK_FALSE(parsePreset(R"({"format":1,"name":"X","bindings":{"edit.undo":{"keys":["Bogus"]}}})"));
}

TEST_CASE("legacy select_tool context loads and new tool scopes serialize") {
    const auto old = parsePreset(R"({"format":1,"name":"Old","bindings":{
        "gizmo.mode.translate":{"keys":["W"],"context":"select_tool"}}})");
    REQUIRE(old);
    CHECK(old->bindings.at("gizmo.mode.translate").context == BindingContext::SelectToolActive);
    CHECK(serializePreset(*old).find("tool:tool.select") != std::string::npos);

    auto preset = hammerPreset();
    preset.bindings.at("vertex.mode.vertex") = Binding{
        std::vector<KeyChord>{chord("1")}, BindingContext::forTool("tool.vertex")};
    const auto loaded = parsePreset(serializePreset(preset));
    REQUIRE(loaded);
    CHECK(loaded->bindings.at("vertex.mode.vertex").context == BindingContext::forTool("tool.vertex"));
}

TEST_CASE("preset names") {
    CHECK(checkPresetName("My keys 2"));
    CHECK(*checkPresetName("trail  ") == "trail");
    for (const char* bad : {"", " x", ".x", "a/b", "a\\b", "..", "Hammer", "blender", "a:b"})
        CHECK_FALSE(checkPresetName(bad));
    CHECK_FALSE(checkPresetName(std::string(65, 'a')));
}

TEST_CASE("assigning a chord takes it from its previous owner") {
    Preset preset = hammerPreset();
    CHECK(chordOwner(preset, chord("Ctrl+Z")) == "edit.undo");
    CHECK_FALSE(chordOwner(preset, chord("Ctrl+Z"), "edit.undo"));
    const auto lost = assignChords(preset, "edit.redo", {chord("Ctrl+Z")});
    CHECK(lost == std::vector<ActionId>{"edit.undo"});
    CHECK(std::holds_alternative<Unbound>(preset.bindings.at("edit.undo").value));
    CHECK(validate(preset).empty());
    CHECK(Keymap(preset).lookup(chord("Ctrl+Z")) == "edit.redo");

    // Rebinding retains the action's default tool scope.
    Preset blender = blenderPreset();
    assignChords(blender, "gizmo.mode.translate", {chord("W")});
    CHECK(blender.bindings.at("gizmo.mode.translate").context == BindingContext::SelectToolActive);
    CHECK(validate(blender).empty());

    // P0 (0.9.8): the Blender preset's tool-scoped bindings keep their scope through clear and rebind,
    // and a chord is only "owned" within its own context.
    Preset scoped = blenderPreset();
    REQUIRE(scoped.bindings.at("apply.click").context == BindingContext::forTool("tool.apply"));
    assignChords(scoped, "apply.click", {});
    CHECK(std::holds_alternative<Unbound>(scoped.bindings.at("apply.click").value));
    assignChords(scoped, "apply.click", {chord("Alt")});
    CHECK(scoped.bindings.at("apply.click").context == BindingContext::forTool("tool.apply"));
    CHECK(chordOwner(scoped, chord("1"), "vertex.mode.vertex", BindingContext::Always) == "selection.mode.groups");
    CHECK(chordOwner(scoped, chord("1"), "selection.mode.groups", BindingContext::forTool("tool.vertex")) == "vertex.mode.vertex");
    CHECK(validate(scoped).empty());

    unbindAction(preset, "edit.redo");
    CHECK(validate(preset).empty());
    CHECK(allowsHeldModifier("snap.suspend"));
    CHECK_FALSE(allowsHeldModifier("edit.undo"));
}

TEST_CASE("the preset store saves, lists, loads and removes user presets") {
    TempDir dir;
    PresetStore store(dir.path);
    CHECK(store.list().empty());

    Preset mine = hammerPreset();
    mine.name = "Mine";
    assignChords(mine, "edit.redo", {chord("Ctrl+Shift+Z")});
    REQUIRE(store.save(mine));
    CHECK(store.list() == std::vector<std::string>{"Mine"});

    const auto loaded = store.find("Mine");
    REQUIRE(loaded);
    CHECK(std::get<std::vector<KeyChord>>(loaded->bindings.at("edit.redo").value) ==
          std::vector<KeyChord>{chord("Ctrl+Shift+Z")});
    CHECK(store.find("Hammer")->name == "Hammer");
    CHECK_FALSE(store.find("Missing"));

    Preset builtinName = mine;
    builtinName.name = "Hammer";
    CHECK_FALSE(store.save(builtinName));
    builtinName.name = "../escape";
    CHECK_FALSE(store.save(builtinName));

    REQUIRE(store.remove("Mine"));
    CHECK(store.list().empty());
    CHECK_FALSE(store.remove("Mine"));
}

TEST_CASE("P0 missing plugin action binding survives preset round trip") {
    const auto parsed = parsePreset(R"({"format":1,"name":"Plugin","bindings":{
        "plugin.example.hello":{"keys":["F24"],"context":"always"},
        "no.such.action":{"keys":["F23"]}}})");
    REQUIRE(parsed);
    REQUIRE(parsed->bindings.contains("plugin.example.hello"));
    CHECK_FALSE(parsed->bindings.contains("no.such.action"));
    CHECK(validate(*parsed).empty());
    CHECK_FALSE(Keymap(*parsed).lookup(chord("F24")));
    const auto restored = parsePreset(serializePreset(*parsed));
    REQUIRE(restored);
    REQUIRE(restored->bindings.contains("plugin.example.hello"));
    CHECK(std::get<std::vector<KeyChord>>(restored->bindings.at("plugin.example.hello").value) ==
          std::vector<KeyChord>{chord("F24")});
}

TEST_CASE("dormant plugin binding does not consume a built-in chord") {
    const auto parsed = parsePreset(R"({"format":1,"name":"Dormant","bindings":{
        "edit.undo":{"keys":["Ctrl+Z"]},
        "plugin.example.hello":{"keys":["Ctrl+Z"]}}})");
    REQUIRE(parsed);
    CHECK(Keymap(*parsed).lookup(chord("Ctrl+Z")) == "edit.undo");
    CHECK(parsed->bindings.contains("plugin.example.hello"));
}

TEST_CASE("preset pie layouts survive serialization") {
    auto preset = hammerPreset();
    preset.pies["pie.tools"] = PieMenu{"Custom tools", {{"tool.rope", PieSlot::N, "Rope"}}};
    const auto loaded = parsePreset(serializePreset(preset));
    REQUIRE(loaded);
    REQUIRE(loaded->pies.contains("pie.tools"));
    CHECK(loaded->pies.at("pie.tools").title == "Custom tools");
    REQUIRE(loaded->pies.at("pie.tools").items.size() == 1);
    CHECK(loaded->pies.at("pie.tools").items[0].action == "tool.rope");
    CHECK(loaded->pies.at("pie.tools").items[0].slot == PieSlot::N);
}

TEST_CASE("P1 preset pies reject incomplete and conflicting layouts") {
    for (int defect = 0; defect < 3; ++defect) {
        CAPTURE(defect);
        auto preset = hammerPreset();
        PieMenu menu{"Invalid", {{"tool.rope", PieSlot::N, "Rope"}}};
        if (defect == 0) menu.items[0].slot.reset();
        if (defect == 1) menu.items.push_back(menu.items[0]);
        if (defect == 2) for (int i = 1; i < 9; ++i)
            menu.items.push_back({"tool.rope", static_cast<PieSlot>(i % 8), "Rope"});
        preset.pies["pie.tools"] = menu;
        const auto parsed = parsePreset(serializePreset(preset));
        CHECK_FALSE(parsed);
        if (!parsed) CHECK(parsed.error().what() != std::string{});
    }
}

TEST_CASE("an edited pie round-trips through the preset store, dormant items included") {
    TempDir dir;
    PresetStore store(dir.path);
    Preset mine = hammerPreset();
    mine.name = "Pies";
    CHECK(serializePreset(mine).find("pies") == std::string::npos);  // No layouts: nothing stored, defaults apply.
    mine.pies["pie.tools"] = PieMenu{"My tools", {{"tool.rope", PieSlot::N, "Rope"},
                                                  {"plugin.absent.action", PieSlot::SE, "Gone"},
                                                  {"edit.undo", PieSlot::W, "Undo"}}};
    REQUIRE(store.save(mine));
    const auto loaded = store.find("Pies");
    REQUIRE(loaded);
    REQUIRE(loaded->pies.contains("pie.tools"));
    const auto& menu = loaded->pies.at("pie.tools");
    CHECK(menu.title == "My tools");
    REQUIRE(menu.items.size() == 3);
    CHECK(pieItemAt(menu, PieSlot::SE)->action == "plugin.absent.action");  // Kept although no plugin provides it.
    CHECK(pieItemAt(menu, PieSlot::W)->label == "Undo");
    CHECK_FALSE(loaded->pies.contains("pie.objects"));
    CHECK(effectivePie("pie.objects", *loaded).items.size() == objectPie().items.size());
}

TEST_CASE("the preset store refuses a pie the loader would reject") {
    TempDir dir;
    PresetStore store(dir.path);
    Preset bad = hammerPreset();
    bad.name = "BadPie";
    bad.pies["pie.tools"] = PieMenu{"Dup", {{"tool.rope", PieSlot::N, "A"}, {"tool.clip", PieSlot::N, "B"}}};
    const auto saved = store.save(bad);
    CHECK_FALSE(saved);
    CHECK(store.list().empty());
}
