#pragma once

#include "core/result.hpp"

#include <compare>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace apex::editor {

using ActionId = std::string;

struct Action {
    ActionId id;
    std::string name;
    std::string category;
    std::string description;
};

// Entries retain declaration order, independently of a preset's map order.
class Actions {
public:
    Actions();
    explicit Actions(std::vector<Action> entries);
    const std::vector<Action>& entries() const { return entries_; }
    const Action* find(std::string_view id) const;
    // Registrations are appended in order; duplicates and changes after freeze are refused.
    bool add(Action action, std::function<void()> invoke = {});
    bool invoke(std::string_view id) const;
    void freeze() { frozen_ = true; }
    bool frozen() const { return frozen_; }

private:
    std::vector<Action> entries_;
    bool frozen_ = false;
    std::map<ActionId, std::function<void()>, std::less<>> callbacks_;
};

using ActionRegistry = Actions;
ActionRegistry& mutableActionRegistry();
const Actions& actionRegistry();

struct KeyChord {
    std::string key;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    // Canonical names include BracketLeft/BracketRight; [ and ] are accepted aliases.
    // A modifier by itself (Ctrl or Alt) represents a held key, e.g. snap.suspend.
    static Result<KeyChord> parse(std::string_view text);
    std::string toString() const;
    auto operator<=>(const KeyChord&) const = default;
};

// Empty toolId is global. Tool-scoped bindings take precedence over global bindings.
struct BindingContext {
    std::string toolId;
    static const BindingContext Always;
    static const BindingContext SelectToolActive;
    static BindingContext forTool(std::string toolId) { return {std::move(toolId)}; }
    auto operator<=>(const BindingContext&) const = default;
};

struct Unbound {
    std::string reason;
};

struct Binding {
    std::variant<Unbound, std::vector<KeyChord>> value;
    BindingContext context = BindingContext::Always;
    bool provisional = false;
};

enum class PieSlot { N, NE, E, SE, S, SW, W, NW };
struct PieItem {
    ActionId action;
    std::optional<PieSlot> slot;
    std::string label;
};
struct PieMenu {
    std::string title;
    std::vector<PieItem> items;
};

struct Preset {
    std::string name;
    std::map<ActionId, Binding, std::less<>> bindings;
    std::map<ActionId, PieMenu, std::less<>> pies;
};

const Preset& hammerPreset(); // Default preset.
const Preset& blenderPreset();
const Preset* findPreset(std::string_view name);
std::vector<std::string_view> presetNames();
std::vector<std::string> validate(const Preset& preset, const Actions& actions = actionRegistry());

class Keymap {
public:
    // Throws invalid_argument with validation problems; never resolves conflicts silently.
    explicit Keymap(const Preset& preset, const Actions& actions = actionRegistry());
    std::optional<ActionId> lookup(const KeyChord& chord,
                                  BindingContext activeTool = BindingContext::Always) const;

private:
    std::map<std::pair<BindingContext, KeyChord>, ActionId> entries_;
};

} // namespace apex::editor
