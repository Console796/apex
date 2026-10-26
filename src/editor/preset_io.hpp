#pragma once

#include "editor/actions.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apex::editor {

// User binding presets: the JSON form of a Preset, plus the edits the keybind editor makes.

std::string serializePreset(const Preset& preset);
// Unknown actions are dropped and actions the file lacks are left unbound, so a preset saved by
// an older build still loads. The result is validated; a duplicate chord is an error.
Result<Preset> parsePreset(std::string_view text, const Actions& actions = actionRegistry());

// Names become file names: 1-64 of letters, digits, space, '-', '_' or '.', not starting with a
// dot or space, and never one of the built-in preset names (compared case-insensitively).
bool isBuiltinPresetName(std::string_view name);
Result<std::string> checkPresetName(std::string_view name);

// The action that already holds this chord, if any (the action `except` is ignored).
std::optional<ActionId> chordOwner(const Preset& preset, const KeyChord& chord,
                                   std::string_view except = {},
                                   BindingContext context = BindingContext::Always);
// Replaces the action's chords. A chord held by another action is taken from it; an action left
// with no chord becomes unbound. Returns the actions that lost a chord.
std::vector<ActionId> assignChords(Preset& preset, const ActionId& id, std::vector<KeyChord> chords);
void unbindAction(Preset& preset, const ActionId& id, std::string reason = "Unbound by the user.");
// Whether the action may be bound to a bare Ctrl/Alt/Shift (a held key).
bool allowsHeldModifier(const ActionId& id);
// The built-in preset's value for this action, used as the starting point for a reset.
const Binding& defaultBinding(const ActionId& id);

class PresetStore {
public:
    explicit PresetStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
    const std::filesystem::path& directory() const { return directory_; }

    // Names of valid saved presets, sorted. Unreadable files are skipped.
    std::vector<std::string> list() const;
    Result<Preset> load(std::string_view name) const;
    Result<void> save(const Preset& preset) const;
    Result<void> remove(std::string_view name) const;
    // Preset by name: user presets first, then the built-ins.
    Result<Preset> find(std::string_view name) const;

private:
    std::filesystem::path pathFor(std::string_view name) const;
    std::filesystem::path directory_;
};

} // namespace apex::editor
