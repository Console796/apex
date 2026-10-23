#include "editor/actions.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace apex::editor {

Actions::Actions()
    : entries_{
        {"edit.undo", "Undo", "Edit", "Undo the last edit and restore its selection."},
        {"edit.redo", "Redo", "Edit", "Redo the last undone edit and restore its selection."},
        {"selection.all", "Select all", "Selection", "Select all visible targets in the current mode."},
        {"selection.none", "Select none", "Selection", "Unpin the work plane, or clear the selection."},
        {"selection.invert", "Invert selection", "Selection",
         "Invert the selection of visible targets."},
        {"selection.mode.groups", "Groups", "Selection", "Select groups as units."},
        {"selection.mode.objects", "Objects", "Selection", "Select objects."},
        {"selection.mode.solids", "Solids", "Selection", "Select individual solids."},
        {"selection.mode.faces", "Faces", "Selection", "Select individual faces."},
        {"selection.ignore_groups", "Ignore groups", "Selection",
         "Toggle selection through group membership."},
        {"selection.box", "Box select", "Selection", "Start a marquee selection."},
        {"selection.marquee_toggle", "Marquee mode", "Selection",
         "Toggle Enclosed/Touching marquee selection."},
        {"edit.delete", "Delete", "Edit", "Delete the selected objects."},
        {"edit.duplicate", "Duplicate", "Edit", "Duplicate the selected objects."},
        {"edit.copy", "Copy", "Edit", "Copy the selected objects to the clipboard."},
        {"edit.cut", "Cut", "Edit", "Copy the selected objects to the clipboard and delete them."},
        {"edit.paste", "Paste", "Edit", "Paste the clipboard at the work-plane position."},
        {"edit.paste_in_place", "Paste in place", "Edit",
         "Paste the clipboard at the objects' original position."},
        {"edit.paste_special", "Paste special", "Edit",
         "Paste the clipboard as several offset or rotated copies."},
        {"edit.group", "Group", "Edit", "Group the selected objects."},
        {"edit.ungroup", "Ungroup", "Edit", "Ungroup the selected groups."},
        {"edit.tie_to_entity", "Tie to entity", "Edit", "Tie the selected solids to a brush entity."},
        {"edit.move_to_world", "Move to world", "Edit",
         "Move selected brush entity solids to the world."},
        {"view.hide", "Hide", "View", "Hide the selected objects."},
        {"view.unhide", "Unhide all", "View", "Unhide all hidden objects."},
        {"view.isolate", "Isolate", "View", "Toggle session isolation of the selection."},
        {"view.hide_unselected", "Hide unselected", "View", "Hide objects outside the selection."},
        {"grid.larger", "Larger grid", "Grid", "Increase the grid spacing."},
        {"grid.smaller", "Smaller grid", "Grid", "Decrease the grid spacing."},
        {"workplane.pin", "Pin work plane to hovered surface", "Grid", "Pin work plane to hovered surface."},
        {"workplane.axis_x", "Pin X axis plane", "Grid", "Pin work plane to the X axis plane through the selection."},
        {"workplane.axis_y", "Pin Y axis plane", "Grid", "Pin work plane to the Y axis plane through the selection."},
        {"workplane.axis_z", "Pin Z axis plane", "Grid", "Pin work plane to the Z axis plane through the selection."},
        {"snap.suspend", "Suspend snapping", "Transform",
         "Hold this key to suspend snapping during a transform."},
        {"tool.select", "Select tool", "Tool", "Activate the select tool."},
        {"tool.entity", "Entity tool", "Tool", "Activate point entity placement."},
        {"gizmo.mode.translate", "Translate gizmo", "Transform",
         "Use the translation gizmo with the select tool."},
        {"gizmo.mode.rotate", "Rotate gizmo", "Transform",
         "Use the rotation gizmo with the select tool."},
        {"gizmo.mode.scale", "Scale gizmo", "Transform", "Use the scale gizmo with the select tool."},
        {"transform.dialog", "Transform dialog", "Transform", "Enter exact transform values."},
        {"transform.modal.grab", "Grab", "Transform",
         "Start modal translation. X/Y/Z constrains an axis (twice: local); Shift+X/Y/Z "
         "excludes that axis. Digits set a value; Enter/left click commits; Esc/right click "
         "cancels."},
        {"transform.modal.rotate", "Rotate", "Transform",
         "Start modal rotation. X/Y/Z constrains an axis (twice: local); Shift+X/Y/Z excludes "
         "that axis. Digits set a value; Enter/left click commits; Esc/right click cancels."},
        {"transform.modal.scale", "Scale", "Transform",
         "Start modal scaling. X/Y/Z constrains an axis (twice: local); Shift+X/Y/Z excludes "
         "that axis. Digits set a value; Enter/left click commits; Esc/right click cancels."},
        {"gizmo.space.toggle", "Gizmo space", "Transform", "Toggle the gizmo between global and local axes."},
        {"gizmo.pivot.cycle", "Cycle pivot", "Transform",
         "Cycle the transform pivot: bounds centre, active origin, custom point."},
        {"snap.mode.toggle", "Snap mode", "Transform", "Toggle between absolute and relative snapping."},
        {"transform.texture_lock.toggle", "Texture lock", "Transform",
         "Toggle texture lock while transforming."},
        {"edit.flip.x", "Flip X", "Edit", "Mirror the selection across its pivot along X."},
        {"edit.flip.y", "Flip Y", "Edit", "Mirror the selection across its pivot along Y."},
        {"edit.flip.z", "Flip Z", "Edit", "Mirror the selection across its pivot along Z."},
        {"nudge.left", "Nudge left", "Transform", "Move the selection one grid step along -U of the work plane."},
        {"nudge.right", "Nudge right", "Transform", "Move the selection one grid step along +U of the work plane."},
        {"nudge.up", "Nudge up", "Transform", "Move the selection one grid step along +V of the work plane."},
        {"nudge.down", "Nudge down", "Transform", "Move the selection one grid step along -V of the work plane."},
        {"view.properties", "Object properties", "Window",
         "Show the inspector: in its workspace area, or in a floating window (see Preferences)."},
        {"workspace.next", "Next workspace", "Window", "Switch to the next workspace tab."},
        {"workspace.previous", "Previous workspace", "Window", "Switch to the previous workspace tab."},
      } {}

Actions::Actions(std::vector<Action> entries) : entries_(std::move(entries)) {}

const Action* Actions::find(std::string_view id) const {
    const auto it = std::ranges::find(entries_, id, &Action::id);
    return it == entries_.end() ? nullptr : &*it;
}

const Actions& actionRegistry() {
    static const Actions actions;
    return actions;
}

namespace {
bool validKey(std::string_view key) {
    if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') ||
                            (key[0] >= '0' && key[0] <= '9')))
        return true;
    for (std::string_view name : {"Delete", "Backspace", "BracketLeft", "BracketRight",
                                 "Enter", "Escape", "Space", "Tab", "Insert", "Home", "End",
                                 "PageUp", "PageDown", "Left", "Right", "Up", "Down",
                                 "Ctrl", "Shift", "Alt"}) {
        if (key == name)
            return true;
    }
    for (int i = 1; i <= 24; ++i) {
        if (key == "F" + std::to_string(i))
            return true;
    }
    return false;
}
}

Result<KeyChord> KeyChord::parse(std::string_view text) {
    const auto invalid = [] { return std::unexpected(Error{ErrorCode::ParseError, "Invalid key chord"}); };
    KeyChord chord;
    while (true) {
        const auto plus = text.find('+');
        const auto part = text.substr(0, plus);
        if (plus == std::string_view::npos) {
            chord.key = part == "[" ? "BracketLeft" : part == "]" ? "BracketRight" : part;
            break;
        }
        bool* modifier = nullptr;
        if (part == "Ctrl") modifier = &chord.ctrl;
        else if (part == "Shift") modifier = &chord.shift;
        else if (part == "Alt") modifier = &chord.alt;
        if (!modifier || *modifier)
            return invalid();
        *modifier = true;
        text.remove_prefix(plus + 1);
    }
    if (!validKey(chord.key) || (chord.key == "Ctrl" && chord.ctrl) ||
        (chord.key == "Shift" && chord.shift) || (chord.key == "Alt" && chord.alt))
        return invalid();
    return chord;
}

std::string KeyChord::toString() const {
    return std::string(ctrl ? "Ctrl+" : "") + (shift ? "Shift+" : "") +
           (alt ? "Alt+" : "") + key;
}

namespace {
Binding bound(std::initializer_list<std::string_view> keys,
              BindingContext context = BindingContext::Always, bool provisional = false) {
    std::vector<KeyChord> chords;
    for (auto key : keys) chords.push_back(KeyChord::parse(key).value());
    return {std::move(chords), context, provisional};
}
Binding unbound(std::string reason) { return {Unbound{std::move(reason)}}; }
}

const Preset& hammerPreset() {
    static const Preset preset{"Hammer", {
        {"edit.undo", bound({"Ctrl+Z"})},
        {"edit.redo", bound({"Ctrl+Y"})},
        {"selection.all", bound({"Ctrl+A"}, BindingContext::Always, true)},
        {"selection.none", bound({"Escape"}, BindingContext::Always, true)},
        {"selection.invert", unbound("Available through the Selection menu.")},
        {"selection.mode.groups", unbound("Choose the mode in the selection controls.")},
        {"selection.mode.objects", unbound("Choose the mode in the selection controls.")},
        {"selection.mode.solids", unbound("Choose the mode in the selection controls.")},
        {"selection.mode.faces", unbound("Choose the mode in the selection controls.")},
        {"selection.ignore_groups", bound({"Ctrl+W"})},
        {"selection.box", unbound("Drag with the select tool to start a marquee.")},
        {"selection.marquee_toggle", unbound("Choose Enclosed/Touching in the selection controls.")},
        {"edit.delete", bound({"Delete"}, BindingContext::Always, true)},
        {"edit.duplicate", bound({"Ctrl+D"}, BindingContext::Always, true)},
        {"edit.copy", bound({"Ctrl+C"})},
        {"edit.cut", bound({"Ctrl+X"})},
        {"edit.paste", bound({"Ctrl+V"})},
        {"edit.paste_in_place", bound({"Ctrl+Shift+V"}, BindingContext::Always, true)},
        {"edit.paste_special", bound({"Ctrl+Alt+V"}, BindingContext::Always, true)},
        {"edit.group", bound({"Ctrl+G"})},
        {"edit.ungroup", bound({"Ctrl+U"})},
        {"edit.tie_to_entity", bound({"Ctrl+T"})},
        {"edit.move_to_world", bound({"Ctrl+Shift+W"})},
        {"view.hide", bound({"H"})},
        {"view.unhide", bound({"Alt+H"}, BindingContext::Always, true)},
        {"view.isolate", bound({"Shift+H"}, BindingContext::Always, true)},
        {"view.hide_unselected", unbound("Use isolate for session visibility, or the View menu to hide.")},
        {"grid.larger", bound({"]"})},
        {"grid.smaller", bound({"["})},
        {"workplane.pin", bound({"Alt+P"}, BindingContext::Always, true)},
        {"workplane.axis_x", unbound("Available through View > Grid.")},
        {"workplane.axis_y", unbound("Available through View > Grid.")},
        {"workplane.axis_z", unbound("Available through View > Grid.")},
        {"snap.suspend", bound({"Alt"})},
        {"tool.select", bound({"Shift+S"})},
        {"tool.entity", bound({"Shift+E"})},
        {"gizmo.mode.translate", bound({"W"}, BindingContext::SelectToolActive)},
        {"gizmo.mode.rotate", bound({"E"}, BindingContext::SelectToolActive)},
        {"gizmo.mode.scale", bound({"R"}, BindingContext::SelectToolActive)},
        {"transform.dialog", bound({"Ctrl+M"})},
        {"transform.modal.grab", unbound("Hammer uses the translate gizmo or transform dialog.")},
        {"transform.modal.rotate", unbound("Hammer uses the rotate gizmo or transform dialog.")},
        {"transform.modal.scale", unbound("Hammer uses the scale gizmo or transform dialog.")},
        {"gizmo.space.toggle", unbound("Choose the space in the transform toolbar.")},
        {"gizmo.pivot.cycle", unbound("Choose the pivot in the transform toolbar.")},
        {"snap.mode.toggle", unbound("Choose the snap mode in the transform toolbar.")},
        {"transform.texture_lock.toggle", bound({"Shift+L"}, BindingContext::Always, true)},
        {"edit.flip.x", bound({"Ctrl+L"}, BindingContext::Always, true)},
        {"edit.flip.y", bound({"Ctrl+I"}, BindingContext::Always, true)},
        {"edit.flip.z", unbound("Available through the Edit menu.")},
        {"nudge.left", bound({"Left"}, BindingContext::SelectToolActive, true)},
        {"nudge.right", bound({"Right"}, BindingContext::SelectToolActive, true)},
        {"nudge.up", bound({"Up"}, BindingContext::SelectToolActive, true)},
        {"nudge.down", bound({"Down"}, BindingContext::SelectToolActive, true)},
        {"view.properties", bound({"Alt+Enter"})},
        {"workspace.next", bound({"Ctrl+PageDown"})},
        {"workspace.previous", bound({"Ctrl+PageUp"})},
    }};
    return preset;
}

const Preset& blenderPreset() {
    static const Preset preset{"Blender", {
        {"edit.undo", bound({"Ctrl+Z"})},
        {"edit.redo", bound({"Ctrl+Shift+Z"})},
        {"selection.all", bound({"A"})},
        {"selection.none", bound({"Alt+A", "Escape"})},
        {"selection.invert", unbound("Available through the Selection menu.")},
        {"selection.mode.groups", unbound("Choose the mode in the selection controls.")},
        {"selection.mode.objects", unbound("Choose the mode in the selection controls.")},
        {"selection.mode.solids", unbound("Choose the mode in the selection controls.")},
        {"selection.mode.faces", unbound("Choose the mode in the selection controls.")},
        {"selection.ignore_groups", unbound("Available through the Selection menu.")},
        {"selection.box", bound({"B"})},
        {"selection.marquee_toggle", unbound("Choose Enclosed/Touching in the selection controls.")},
        {"edit.delete", bound({"X", "Delete"})},
        {"edit.duplicate", bound({"Shift+D"})},
        {"edit.copy", bound({"Ctrl+C"})},
        {"edit.cut", unbound("Blender has no cut; copy, then delete.")},
        {"edit.paste", bound({"Ctrl+V"})},
        {"edit.paste_in_place", unbound("Available through the Edit menu.")},
        {"edit.paste_special", unbound("Available through the Edit menu.")},
        {"edit.group", bound({"Ctrl+G"})},
        {"edit.ungroup", unbound("Available through the Edit menu.")},
        {"edit.tie_to_entity", unbound("Available through the Edit menu.")},
        {"edit.move_to_world", unbound("Available through the Edit menu.")},
        {"view.hide", bound({"H"})},
        {"view.unhide", bound({"Alt+H"})},
        {"view.isolate", unbound("Available through the View menu.")},
        {"view.hide_unselected", bound({"Shift+H"})},
        {"grid.larger", unbound("Choose spacing in the grid controls.")},
        {"grid.smaller", unbound("Choose spacing in the grid controls.")},
        {"workplane.pin", unbound("Available through View > Grid; no Blender surface pin binding.")},
        {"workplane.axis_x", unbound("Available through View > Grid.")},
        {"workplane.axis_y", unbound("Available through View > Grid.")},
        {"workplane.axis_z", unbound("Available through View > Grid.")},
        {"snap.suspend", bound({"Ctrl"})},
        {"tool.select", unbound("Choose the select tool in the toolbar.")},
        {"tool.entity", unbound("Choose the entity tool in the toolbar.")},
        {"gizmo.mode.translate", unbound("Choose the gizmo in the toolbar; G starts modal translation.")},
        {"gizmo.mode.rotate", unbound("Choose the gizmo in the toolbar; R starts modal rotation.")},
        {"gizmo.mode.scale", unbound("Choose the gizmo in the toolbar; S starts modal scaling.")},
        {"transform.dialog", unbound("Available through the Edit menu.")},
        {"transform.modal.grab", bound({"G"})},
        {"transform.modal.rotate", bound({"R"})},
        {"transform.modal.scale", bound({"S"})},
        {"gizmo.space.toggle", bound({"Alt+Space"})},
        {"gizmo.pivot.cycle", unbound("Choose the pivot in the transform toolbar.")},
        {"snap.mode.toggle", bound({"Shift+Tab"})},
        {"transform.texture_lock.toggle", unbound("Available through the Edit menu.")},
        {"edit.flip.x", unbound("Available through the Edit menu; use S then -1 on an axis.")},
        {"edit.flip.y", unbound("Available through the Edit menu; use S then -1 on an axis.")},
        {"edit.flip.z", unbound("Available through the Edit menu; use S then -1 on an axis.")},
        {"nudge.left", unbound("Blender moves with G and typed values; no arrow-key nudge.")},
        {"nudge.right", unbound("Blender moves with G and typed values; no arrow-key nudge.")},
        {"nudge.up", unbound("Blender moves with G and typed values; no arrow-key nudge.")},
        {"nudge.down", unbound("Blender moves with G and typed values; no arrow-key nudge.")},
        {"view.properties", bound({"N"})},
        {"workspace.next", bound({"Ctrl+PageDown"})},
        {"workspace.previous", bound({"Ctrl+PageUp"})},
    }};
    return preset;
}

const Preset* findPreset(std::string_view name) {
    for (const auto* preset : {&hammerPreset(), &blenderPreset()}) {
        if (preset->name == name)
            return preset;
    }
    return nullptr;
}

std::vector<std::string_view> presetNames() { return {hammerPreset().name, blenderPreset().name}; }

std::vector<std::string> validate(const Preset& preset, const Actions& actions) {
    std::vector<std::string> problems;
    for (const auto& action : actions.entries()) {
        if (!preset.bindings.contains(action.id)) problems.push_back("Missing action: " + action.id);
    }
    std::map<KeyChord, ActionId> chords;
    for (const auto& [id, binding] : preset.bindings) {
        if (!actions.find(id)) problems.push_back("Unknown action: " + id);
        if (const auto* unboundValue = std::get_if<Unbound>(&binding.value)) {
            if (unboundValue->reason.find_first_not_of(" \t\r\n") == std::string::npos)
                problems.push_back("Unbound action has no reason: " + id);
            continue;
        }
        const auto& keys = std::get<std::vector<KeyChord>>(binding.value);
        if (keys.empty()) problems.push_back("Empty binding: " + id);
        for (const auto& chord : keys) {
            const auto parsed = KeyChord::parse(chord.toString());
            if (!parsed || *parsed != chord) problems.push_back("Invalid key chord for action: " + id);
            // Both supported contexts overlap when the select tool is active.
            const auto [it, inserted] = chords.emplace(chord, id);
            if (!inserted)
                problems.push_back("Duplicate chord " + chord.toString() + ": " +
                                   it->second + " and " + id);
        }
    }
    return problems;
}

Keymap::Keymap(const Preset& preset, const Actions& actions) {
    const auto problems = validate(preset, actions);
    if (!problems.empty()) {
        std::string message = "Invalid preset " + preset.name;
        for (const auto& problem : problems) message += "\n" + problem;
        throw std::invalid_argument(message);
    }
    for (const auto& [id, binding] : preset.bindings) {
        if (const auto* keys = std::get_if<std::vector<KeyChord>>(&binding.value)) {
            for (const auto& chord : *keys) entries_.emplace(chord, Entry{id, binding.context});
        }
    }
}

std::optional<ActionId> Keymap::lookup(const KeyChord& chord, BindingContext context) const {
    const auto it = entries_.find(chord);
    if (it == entries_.end() || (it->second.context == BindingContext::SelectToolActive &&
                                context != BindingContext::SelectToolActive))
        return std::nullopt;
    return it->second.action;
}

} // namespace apex::editor
