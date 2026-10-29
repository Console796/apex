#include "editor/actions.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace apex::editor {

const BindingContext BindingContext::Always{};
const BindingContext BindingContext::SelectToolActive{"tool.select"};

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
        {"instance.create", "Create Instance from Selection…", "Instancing", "Save selection to a new VMF and replace it with an instance."},
        {"instance.open", "Open Instance", "Instancing", "Open the selected instance in another Apex window."},
        {"instance.collapse", "Collapse Instance", "Instancing", "Replace selected instances with their contents."},
        {"instance.collapse_all", "Collapse All Instances", "Instancing", "Replace all instances with their contents."},
        {"view.instances", "Instance Visibility", "View", "Cycle instance contents between hidden, tinted and normal."},
        {"view.hide", "Hide", "View", "Hide the selected objects."},
        {"view.unhide", "Unhide all", "View", "Unhide all hidden objects."},
        {"view.isolate", "Isolate", "View", "Toggle session isolation of the selection."},
        {"view.hide_unselected", "Hide unselected", "View", "Hide objects outside the selection."},
        {"view.center_selection", "Center views on selection", "View",
         "Move every viewport to the selection without turning it."},
        {"grid.larger", "Larger grid", "Grid", "Increase the grid spacing."},
        {"grid.smaller", "Smaller grid", "Grid", "Decrease the grid spacing."},
        {"workplane.pin", "Pin work plane to hovered surface", "Grid", "Pin work plane to hovered surface."},
        {"workplane.axis_x", "Pin X axis plane", "Grid", "Pin work plane to the X axis plane through the selection."},
        {"workplane.axis_y", "Pin Y axis plane", "Grid", "Pin work plane to the Y axis plane through the selection."},
        {"workplane.axis_z", "Pin Z axis plane", "Grid", "Pin work plane to the Z axis plane through the selection."},
        {"snap.suspend", "Suspend snapping", "Transform",
         "Hold this key to suspend snapping during a transform."},
        {"pie.tools", "Tool pie", "Tool", "Open the tool pie at the cursor."},
        {"tool.select", "Select tool", "Tool", "Activate the select tool."},
        {"edit.carve", "Carve", "Edit", "Subtract selected carvers from visible solids."},
        {"edit.hollow", "Make Hollow…", "Edit", "Replace selected solids with walls."},
        {"assets.reload", "Reload Game Content", "Addons", "Reload game content and addon mounts."},
        {"compile.compile_run", "Compile and Run", "Compile", "Compile the map with the selected profile, then start the game in it."},
        {"compile.run_game", "Run Game", "Compile", "Start the game in the last compiled map without compiling."},
        {"edit.merge", "Merge", "Edit", "Merge selected solids into their convex hull."},
        {"edit.create_prefab", "Create Prefab from Selection…", "Edit",
         "Save the selected objects as a prefab file in the prefab folder."},
        {"tool.entity", "Entity tool", "Tool", "Activate point entity placement."},
        {"block.pick_plane", "Pick block plane", "Tool", "Hold and click a face to pin its plane; empty space unpins."},
        {"tool.primitive", "Primitive tool", "Tool", "Draw wedges, cylinders, spikes, arches and spheres."},
        {"tool.clip", "Clip tool", "Tool", "Place points to clip selected solids; repeat to cycle kept side."},
        {"tool.vertex", "Vertex tool", "Tool", "Edit vertices, edges and faces of selected solids."},
        {"vertex.mode.vertex", "Vertex mode", "Vertex", "Select and move vertices."},
        {"vertex.mode.edge", "Edge mode", "Vertex", "Select and move edges."},
        {"vertex.mode.face", "Face mode", "Vertex", "Select and move faces."},
        {"clip.cycle_keep", "Cycle clip kept side", "Tool", "Keep front, back or both halves."},
        {"tool.apply", "Face edit tool", "Tool", "Edit selected faces; apply materials (Hammer texture application mode)."},
        {"tool.measure", "Measure tool", "Tool", "Measure distance, delta and face angles."},
        {"apply.click", "Apply with click modifier", "Tool", "Hold while clicking to apply; Alt applies with the selected alignment mode."},
        {"apply.lift_aligned", "Lift material with alignment", "Tool", "Hold Shift with Alt-click to lift the material and set the alignment source."},
        {"face.extrapolation.cycle", "Cycle face extrapolation", "Tool", "Cycle Wrap, Projected and Match face alignment modes."},
        {"tool.block", "Block tool", "Tool", "Draw a base and extrude a new solid."},
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
        {"gizmo.pivot.origin.toggle", "Pivot at origin", "Transform",
         "Rotate and scale a single selected object about its own origin, as Hammer does, instead of the centre "
         "of its bounds."},
        {"view.model_overlay.toggle", "Model wireframe", "View",
         "Draw model triangle edges over models in the Flat view and on selected models. The Wireframe view always "
         "draws them."},
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
        {"edit.carve", bound({"Ctrl+Shift+C"})},
        {"edit.hollow", unbound("Available through the Edit menu.")},
        {"assets.reload", unbound("Available through the menu.")},
        {"compile.compile_run", bound({"F5"})},
        {"compile.run_game", bound({"Ctrl+F5"})},
        {"edit.merge", unbound("Available through the Edit menu.")},
        {"edit.create_prefab", unbound("Available through the Edit menu and the Prefabs tab.")},
        {"edit.redo", bound({"Ctrl+Y"})},
        {"selection.all", bound({"Ctrl+A"}, BindingContext::Always, true)},
        {"selection.none", bound({"Escape"}, BindingContext::Always, true)},
        {"selection.invert", unbound("Available through the Selection menu.")},
        {"selection.mode.groups", bound({"1"})},
        {"selection.mode.objects", bound({"2"})},
        {"selection.mode.solids", bound({"3"})},
        {"selection.mode.faces", bound({"4"})},
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
        {"instance.create", unbound("Available through the Instancing menu and selection context menu.")},
        {"instance.open", unbound("Available through the Instancing menu or inspector.")},
        {"instance.collapse", unbound("Available through the Instancing menu.")},
        {"instance.collapse_all", unbound("Available through the Instancing menu.")},
        {"view.instances", unbound("Available through the View menu.")},
        {"view.hide", bound({"H"})},
        {"view.unhide", bound({"Alt+H"}, BindingContext::Always, true)},
        {"view.isolate", bound({"Shift+H"}, BindingContext::Always, true)},
        {"view.hide_unselected", unbound("Use isolate for session visibility, or the View menu to hide.")},
        {"view.center_selection", bound({"Ctrl+E", "Ctrl+Shift+E"})},
        {"grid.larger", bound({"]"})},
        {"grid.smaller", bound({"["})},
        {"workplane.pin", bound({"Alt+P"}, BindingContext::Always, true)},
        {"workplane.axis_x", unbound("Available through View > Grid.")},
        {"workplane.axis_y", unbound("Available through View > Grid.")},
        {"workplane.axis_z", unbound("Available through View > Grid.")},
        {"snap.suspend", bound({"Alt"})},
        {"block.pick_plane", bound({"Ctrl"})},
        {"tool.apply", bound({"Shift+A"})},
        {"tool.measure", bound({"Shift+M"})},
        {"pie.tools", bound({"Q"})},
        {"apply.click", unbound("Right click applies; Alt+right click uses the selected extrapolation mode.")},
        {"apply.lift_aligned", bound({"Shift"}, BindingContext::forTool("tool.apply"))},
        {"face.extrapolation.cycle", unbound("Cycle the mode in Face Properties, or assign a key here.")},
        {"tool.select", bound({"Shift+S"})},
        {"tool.entity", bound({"Shift+E"})},
        {"tool.block", bound({"Shift+B"})},
        {"tool.primitive", bound({"Shift+P"})},
        {"tool.clip", bound({"Shift+X"})},
        {"tool.vertex", bound({"Shift+V"})},
        {"vertex.mode.vertex", bound({"1"}, BindingContext::forTool("tool.vertex"))},
        {"vertex.mode.edge", bound({"2"}, BindingContext::forTool("tool.vertex"))},
        {"vertex.mode.face", bound({"3"}, BindingContext::forTool("tool.vertex"))},
        {"clip.cycle_keep", unbound("Repeat Shift+X while the clip tool is active.")},
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
        {"gizmo.pivot.origin.toggle", unbound("Choose it in the transform toolbar.")},
        {"view.model_overlay.toggle", unbound("Choose it in the transform toolbar.")},
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
        {"edit.carve", unbound("Available through the Edit menu.")},
        {"edit.hollow", unbound("Available through the Edit menu.")},
        {"assets.reload", unbound("Available through the menu.")},
        {"compile.compile_run", bound({"F12"})},
        {"compile.run_game", bound({"Ctrl+F12"})},
        {"edit.merge", unbound("Available through the Edit menu.")},
        {"edit.create_prefab", unbound("Available through the Edit menu and the Prefabs tab.")},
        {"edit.redo", bound({"Ctrl+Shift+Z"})},
        {"selection.all", bound({"A"})},
        {"selection.none", bound({"Alt+A", "Escape"})},
        {"selection.invert", unbound("Available through the Selection menu.")},
        {"selection.mode.groups", bound({"1"})},
        {"selection.mode.objects", bound({"2"})},
        {"selection.mode.solids", bound({"3"})},
        {"selection.mode.faces", bound({"4"})},
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
        {"instance.create", unbound("Available through the Instancing menu and selection context menu.")},
        {"instance.open", unbound("Available through the Instancing menu or inspector.")},
        {"instance.collapse", unbound("Available through the Instancing menu.")},
        {"instance.collapse_all", unbound("Available through the Instancing menu.")},
        {"view.instances", unbound("Available through the View menu.")},
        {"view.hide", bound({"H"})},
        {"view.unhide", bound({"Alt+H"})},
        {"view.isolate", unbound("Available through the View menu.")},
        {"view.hide_unselected", bound({"Shift+H"})},
        {"view.center_selection", bound({"Ctrl+E"})},
        {"grid.larger", unbound("Choose spacing in the grid controls.")},
        {"grid.smaller", unbound("Choose spacing in the grid controls.")},
        {"workplane.pin", unbound("Available through View > Grid; no Blender surface pin binding.")},
        {"workplane.axis_x", unbound("Available through View > Grid.")},
        {"workplane.axis_y", unbound("Available through View > Grid.")},
        {"workplane.axis_z", unbound("Available through View > Grid.")},
        {"snap.suspend", bound({"Ctrl"})},
        {"block.pick_plane", bound({"Alt"})},
        {"tool.apply", unbound("Use the tool pie (Shift+Space) or the Tools drop-down.")},
        {"tool.measure", unbound("Use the tool pie (Shift+Space) or the Tools drop-down.")},
        {"apply.click", bound({"Ctrl"}, BindingContext::forTool("tool.apply"))},
        {"apply.lift_aligned", bound({"Shift"}, BindingContext::forTool("tool.apply"))},
        {"face.extrapolation.cycle", unbound("Cycle the mode in Face Properties, or assign a key here.")},
        {"tool.select", unbound("Use the tool pie (Shift+Space) or the Tools drop-down.")},
        {"tool.entity", unbound("Use the tool pie (Shift+Space) or the Tools drop-down.")},
        {"tool.block", unbound("Use the tool pie (Shift+Space) or the Tools drop-down.")},
        {"tool.primitive", unbound("Use the tool pie (Shift+Space) or the Tools drop-down.")},
        {"tool.clip", unbound("Use the tool pie (Shift+Space) or the Tools drop-down.")},
        {"tool.vertex", unbound("Use the tool pie (Shift+Space) or the Tools drop-down.")},
        {"vertex.mode.vertex", bound({"1"}, BindingContext::forTool("tool.vertex"))},
        {"vertex.mode.edge", bound({"2"}, BindingContext::forTool("tool.vertex"))},
        {"vertex.mode.face", bound({"3"}, BindingContext::forTool("tool.vertex"))},
        {"clip.cycle_keep", unbound("Repeat the clip tool action to cycle the kept side.")},
        {"gizmo.mode.translate", unbound("Choose the gizmo in the toolbar; G starts modal translation.")},
        {"gizmo.mode.rotate", unbound("Choose the gizmo in the toolbar; R starts modal rotation.")},
        {"gizmo.mode.scale", unbound("Choose the gizmo in the toolbar; S starts modal scaling.")},
        {"transform.dialog", unbound("Available through the Edit menu.")},
        {"transform.modal.grab", bound({"G"})},
        {"transform.modal.rotate", bound({"R"})},
        {"transform.modal.scale", bound({"S"})},
        {"gizmo.space.toggle", bound({"Alt+Space"})},
        {"pie.tools", bound({"Shift+Space"})},
        {"gizmo.pivot.cycle", unbound("Choose the pivot in the transform toolbar.")},
        {"snap.mode.toggle", bound({"Shift+Tab"})},
        {"gizmo.pivot.origin.toggle", unbound("Choose it in the transform toolbar.")},
        {"view.model_overlay.toggle", unbound("Choose it in the transform toolbar.")},
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
    std::map<std::pair<BindingContext, KeyChord>, ActionId> chords;
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
            const auto [it, inserted] = chords.emplace(std::pair{binding.context, chord}, id);
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
            for (const auto& chord : *keys) entries_.emplace(std::pair{binding.context, chord}, id);
        }
    }
}

std::optional<ActionId> Keymap::lookup(const KeyChord& chord, BindingContext context) const {
    if (!context.toolId.empty()) {
        if (const auto it = entries_.find({context, chord}); it != entries_.end()) return it->second;
    }
    if (const auto it = entries_.find({BindingContext::Always, chord}); it != entries_.end()) return it->second;
    return std::nullopt;
}

} // namespace apex::editor
