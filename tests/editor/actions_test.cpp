#include "editor/actions.hpp"

#include <doctest/doctest.h>

#include <array>
#include <set>
#include <stdexcept>

using namespace apex::editor;

namespace {
KeyChord chord(std::string_view text) { return KeyChord::parse(text).value(); }

bool hasProblem(const std::vector<std::string>& problems, std::string_view text) {
    for (const auto& problem : problems) {
        if (problem.find(text) != std::string::npos) return true;
    }
    return false;
}

void checkBindings(const Preset& preset,
                   std::initializer_list<std::pair<std::string_view, std::string_view>> expected,
                   BindingContext context = BindingContext::Always) {
    Keymap keymap(preset);
    for (const auto& [key, action] : expected) {
        CAPTURE(key);
        CHECK(keymap.lookup(chord(key), context) == action);
    }
}
}

TEST_CASE("Actions registry has stable ordered unique ids and complete metadata") {
    Actions copy;
    const auto& registry = actionRegistry();
    REQUIRE(registry.entries().size() == 83);
    CHECK(registry.entries().front().id == "edit.undo");
    CHECK(registry.entries().back().id == "workspace.previous");
    std::set<ActionId> ids;
    for (std::size_t i = 0; i < registry.entries().size(); ++i) {
        const auto& action = registry.entries()[i];
        CHECK(ids.insert(action.id).second);
        CHECK(copy.entries()[i].id == action.id);
        CHECK(registry.find(action.id) == &action);
        CHECK_FALSE(action.name.empty());
        CHECK_FALSE(action.category.empty());
        CHECK_FALSE(action.description.empty());
    }
    CHECK(registry.find("unknown.action") == nullptr);
    for (const auto* id : {"transform.modal.grab", "transform.modal.rotate", "transform.modal.scale"}) {
        const auto& description = registry.find(id)->description;
        CHECK(description.find("X/Y/Z") != std::string::npos);
        CHECK(description.find("Shift+X/Y/Z") != std::string::npos);
        CHECK(description.find("local") != std::string::npos);
        CHECK(description.find("commits") != std::string::npos);
        CHECK(description.find("cancels") != std::string::npos);
    }
}

TEST_CASE("Pie and selection mode bindings resolve with tool scopes") {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        CAPTURE(preset->name);
        Keymap keymap(*preset);
        const auto pieKey = preset == &hammerPreset() ? "Q" : "Shift+Space";
        CHECK(keymap.lookup(chord(pieKey)) == "pie.tools");
        const std::array<std::pair<const char*, const char*>, 4> modes{{
            {"1", "selection.mode.groups"}, {"2", "selection.mode.objects"},
            {"3", "selection.mode.solids"}, {"4", "selection.mode.faces"},
        }};
        for (const auto& [key, action] : modes) CHECK(keymap.lookup(chord(key)) == action);
        CHECK(validate(*preset).empty());
        CHECK(keymap.lookup(chord("1"), BindingContext::forTool("tool.vertex")) == "vertex.mode.vertex");
        CHECK(keymap.lookup(chord("2"), BindingContext::forTool("tool.vertex")) == "vertex.mode.edge");
        CHECK(keymap.lookup(chord("3"), BindingContext::forTool("tool.vertex")) == "vertex.mode.face");
        CHECK(keymap.lookup(chord("4"), BindingContext::forTool("tool.vertex")) == "selection.mode.faces");
    }
    const auto* pieAction = actionRegistry().find("pie.tools");
    REQUIRE(pieAction);
    CHECK(pieAction->name == "Tool pie");
    CHECK(pieAction->category == "Tool");
    CHECK(pieAction->description == "Open the tool pie at the cursor.");
    CHECK(std::get<Unbound>(blenderPreset().bindings.at("tool.select").value).reason ==
          "Use the tool pie (Shift+Space) or the Tools drop-down.");
}

TEST_CASE("KeyChord parsing is portable canonical and reversible") {
    for (const auto* text : {"Z", "Ctrl+Shift+W", "Ctrl+Alt+Delete", "Shift+Alt+A", "Delete",
                             "BracketLeft", "BracketRight", "[", "]", "Ctrl+[", "Alt+]",
                             "Ctrl", "Alt", "Shift", "F1", "Ctrl+F24", "0", "Escape"}) {
        CAPTURE(text);
        const auto parsed = KeyChord::parse(text);
        REQUIRE(parsed);
        const auto roundTrip = KeyChord::parse(parsed->toString());
        REQUIRE(roundTrip);
        CHECK(*roundTrip == *parsed);
    }
    CHECK(chord("[").toString() == "BracketLeft");
    CHECK(chord("]").toString() == "BracketRight");
    CHECK(chord("Alt+Shift+Ctrl+W").toString() == "Ctrl+Shift+Alt+W");
    CHECK(chord("Ctrl+Shift+W") == KeyChord{"W", true, true, false});
    for (const auto* text : {"", "+", "Ctrl+", "Ctrl++Z", "Ctrl+Ctrl+Z", "Ctrl+Ctrl",
                             "A+B", "Meta+Z", "Unknown", "F25", "ctrl+Z", "Ctrl+ Z"}) {
        CAPTURE(text);
        CHECK_FALSE(KeyChord::parse(text));
    }
}

TEST_CASE("Both presets explicitly cover every registered action without conflicts") {
    CHECK(presetNames() == std::vector<std::string_view>{"Hammer", "Blender"});
    CHECK(findPreset("Hammer") == &hammerPreset());
    CHECK(findPreset("Blender") == &blenderPreset());
    CHECK(findPreset("unknown") == nullptr);
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        CAPTURE(preset->name);
        const auto problems = validate(*preset);
        for (const auto& problem : problems) INFO(problem);
        CHECK(problems.empty());
        CHECK(preset->bindings.size() == actionRegistry().entries().size());
        for (const auto& action : actionRegistry().entries()) {
            const auto it = preset->bindings.find(action.id);
            REQUIRE(it != preset->bindings.end());
            if (const auto* value = std::get_if<Unbound>(&it->second.value)) {
                CHECK_FALSE(value->reason.empty());
            } else {
                CHECK_FALSE(std::get<std::vector<KeyChord>>(it->second.value).empty());
            }
        }
    }
}

TEST_CASE("Hammer preset follows D15 with select tool gizmo context") {
    checkBindings(hammerPreset(), {
        {"Shift+S", "tool.select"}, {"Shift+E", "tool.entity"}, {"Shift+B", "tool.block"}, {"Shift+X", "tool.clip"},
        {"Shift+V", "tool.vertex"}, {"Shift+P", "tool.primitive"},
        {"Shift+A", "tool.apply"}, {"Shift+M", "tool.measure"},
        {"Ctrl+T", "edit.tie_to_entity"}, {"Ctrl+Shift+W", "edit.move_to_world"},
        {"Ctrl+G", "edit.group"}, {"Ctrl+U", "edit.ungroup"},
        {"Ctrl+C", "edit.copy"}, {"Ctrl+X", "edit.cut"}, {"Ctrl+V", "edit.paste"},
        {"Ctrl+Shift+V", "edit.paste_in_place"}, {"Ctrl+Alt+V", "edit.paste_special"},
        {"Ctrl+W", "selection.ignore_groups"}, {"H", "view.hide"},
        {"Ctrl+E", "view.center_selection"}, {"Ctrl+Shift+E", "view.center_selection"},
        {"Ctrl+M", "transform.dialog"}, {"[", "grid.smaller"}, {"]", "grid.larger"},
        {"Alt+P", "workplane.pin"},
        {"Ctrl+Z", "edit.undo"}, {"Ctrl+Y", "edit.redo"}, {"Alt", "snap.suspend"}, {"Ctrl", "block.pick_plane"},
    });
    checkBindings(hammerPreset(), {
        {"W", "gizmo.mode.translate"}, {"E", "gizmo.mode.rotate"}, {"R", "gizmo.mode.scale"},
        {"H", "view.hide"}, {"Ctrl+Z", "edit.undo"},
    }, BindingContext::SelectToolActive);
    checkBindings(hammerPreset(), {
        {"Left", "nudge.left"}, {"Right", "nudge.right"}, {"Up", "nudge.up"}, {"Down", "nudge.down"},
    }, BindingContext::SelectToolActive);
    checkBindings(hammerPreset(), {
        {"Shift+L", "transform.texture_lock.toggle"}, {"Ctrl+L", "edit.flip.x"}, {"Ctrl+I", "edit.flip.y"},
    });
    Keymap keymap(hammerPreset());
    CHECK_FALSE(keymap.lookup(chord("Left")));
    for (const auto* key : {"W", "E", "R"}) CHECK_FALSE(keymap.lookup(chord(key)));
    CHECK_FALSE(keymap.lookup(chord("F24")));
    for (const auto& [id, binding] : hammerPreset().bindings) {
        const bool provisional = id == "selection.all" || id == "selection.none" ||
                                 id == "edit.delete" || id == "edit.duplicate" ||
                                 id == "edit.paste_in_place" || id == "edit.paste_special" ||
                                 id == "view.unhide" || id == "view.isolate" || id == "workplane.pin" ||
                                 id == "transform.texture_lock.toggle" || id == "edit.flip.x" ||
                                 id == "edit.flip.y" || id.starts_with("nudge.");
        CHECK(binding.provisional == provisional);
    }
}

TEST_CASE("Blender preset follows D15 including alternatives and held snap modifier") {
    checkBindings(blenderPreset(), {
        {"A", "selection.all"}, {"Alt+A", "selection.none"}, {"B", "selection.box"},
        {"Escape", "selection.none"},
        {"H", "view.hide"}, {"Alt+H", "view.unhide"}, {"Shift+H", "view.hide_unselected"},
        {"Shift+D", "edit.duplicate"}, {"X", "edit.delete"}, {"Delete", "edit.delete"},
        {"Ctrl+G", "edit.group"}, {"G", "transform.modal.grab"},
        {"R", "transform.modal.rotate"}, {"S", "transform.modal.scale"},
        {"Ctrl", "snap.suspend"}, {"Alt", "block.pick_plane"}, {"Ctrl+Z", "edit.undo"}, {"Ctrl+Shift+Z", "edit.redo"},
        {"Alt+Space", "gizmo.space.toggle"}, {"Shift+Tab", "snap.mode.toggle"},
    });
    for (const auto& [id, binding] : blenderPreset().bindings) {
        CAPTURE(id);
        CHECK_FALSE(binding.provisional);
    }
}

TEST_CASE("Validation catches registry growth unknown actions and incomplete bindings") {
    auto entries = actionRegistry().entries();
    entries.push_back({"test.fake", "Fake", "Test", "An action added after the presets."});
    Actions extended(std::move(entries));
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        CHECK(hasProblem(validate(*preset, extended), "Missing action: test.fake"));
        CHECK_THROWS_AS((void)Keymap(*preset, extended), std::invalid_argument);
    }
    auto preset = hammerPreset();
    preset.bindings.erase("edit.undo");
    preset.bindings.emplace("test.unknown", Binding{Unbound{"Test only."}});
    preset.bindings.at("edit.redo") = Binding{Unbound{" \t\n"}};
    preset.bindings.at("edit.delete") = Binding{std::vector<KeyChord>{}};
    preset.bindings.at("edit.duplicate") = Binding{std::vector<KeyChord>{{"invalid"}}};
    const auto problems = validate(preset);
    CHECK(hasProblem(problems, "Missing action: edit.undo"));
    CHECK(hasProblem(problems, "Unknown action: test.unknown"));
    CHECK(hasProblem(problems, "Unbound action has no reason: edit.redo"));
    CHECK(hasProblem(problems, "Empty binding: edit.delete"));
    CHECK(hasProblem(problems, "Invalid key chord for action: edit.duplicate"));
    CHECK_THROWS_AS((void)Keymap(preset), std::invalid_argument);
}

TEST_CASE("Duplicate chords conflict in all overlapping binding contexts") {
    auto preset = hammerPreset();
    preset.bindings.at("edit.redo") = preset.bindings.at("edit.undo");
    CHECK(hasProblem(validate(preset), "Duplicate chord Ctrl+Z"));
    CHECK_THROWS_AS((void)Keymap(preset), std::invalid_argument);
    preset = hammerPreset();
    preset.bindings.at("edit.undo") = Binding{std::vector<KeyChord>{chord("W")}, BindingContext::SelectToolActive};
    CHECK(hasProblem(validate(preset), "Duplicate chord W"));
    CHECK_THROWS_AS((void)Keymap(preset), std::invalid_argument);
    preset = hammerPreset();
    preset.bindings.at("edit.undo") = Binding{std::vector<KeyChord>{chord("Ctrl+Z"), chord("Ctrl+Z")}};
    CHECK(hasProblem(validate(preset), "Duplicate chord Ctrl+Z"));
}

TEST_CASE("Tool scopes shadow global bindings and allow reuse in separate scopes") {
    auto preset = hammerPreset();
    preset.bindings.at("selection.mode.groups") = Binding{std::vector<KeyChord>{chord("1")}};
    preset.bindings.at("vertex.mode.vertex") = Binding{std::vector<KeyChord>{chord("1")},
        BindingContext::forTool("tool.vertex")};
    preset.bindings.at("vertex.mode.edge") = Binding{std::vector<KeyChord>{chord("1")},
        BindingContext::forTool("tool.clip")};
    CHECK(validate(preset).empty());
    Keymap keymap(preset);
    CHECK(keymap.lookup(chord("1")) == "selection.mode.groups");
    CHECK(keymap.lookup(chord("1"), BindingContext::forTool("tool.vertex")) == "vertex.mode.vertex");
    CHECK(keymap.lookup(chord("1"), BindingContext::forTool("tool.clip")) == "vertex.mode.edge");
    CHECK(keymap.lookup(chord("1"), BindingContext::forTool("tool.select")) == "selection.mode.groups");
}
