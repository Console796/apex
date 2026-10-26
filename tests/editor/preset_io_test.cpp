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
