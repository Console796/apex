#include "editor/preset_io.hpp"

#include "core/json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace apex::editor {

namespace {
using json::Value;

constexpr double kFormat = 1;

std::string contextName(const BindingContext& context) {
    return context.toolId.empty() ? "always" : "tool:" + context.toolId;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return std::tolower(c); });
    return result;
}

Error bad(std::string message) { return Error{ErrorCode::ParseError, std::move(message)}; }
}

std::string serializePreset(const Preset& preset) {
    Value::Object bindings;
    for (const auto& [id, binding] : preset.bindings) {
        Value::Object entry;
        if (const auto* unboundValue = std::get_if<Unbound>(&binding.value)) {
            entry["unbound"] = Value{unboundValue->reason};
        } else {
            Value::Array keys;
            for (const auto& chord : std::get<std::vector<KeyChord>>(binding.value))
                keys.push_back(Value{chord.toString()});
            entry["keys"] = Value{std::move(keys)};
        }
        entry["context"] = Value{std::string(contextName(binding.context))};
        if (binding.provisional) entry["provisional"] = Value{true};
        bindings[id] = Value{std::move(entry)};
    }
    Value::Object root;
    root["format"] = Value{kFormat};
    root["name"] = Value{preset.name};
    root["bindings"] = Value{std::move(bindings)};
    return json::write(Value{std::move(root)});
}

Result<Preset> parsePreset(std::string_view text, const Actions& actions) {
    auto parsed = json::parse(text);
    if (!parsed) return std::unexpected(parsed.error());
    const auto* root = parsed->get<Value::Object>();
    if (!root) return std::unexpected(bad("Preset is not a JSON object"));
    const auto field = [&](const Value::Object& object, std::string_view key) -> const Value* {
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    };
    const auto* format = field(*root, "format");
    if (!format || !format->get<double>() || *format->get<double>() != kFormat)
        return std::unexpected(bad("Unsupported preset format"));
    const auto* name = field(*root, "name");
    if (!name || !name->get<std::string>()) return std::unexpected(bad("Preset has no name"));
    const auto* bindings = field(*root, "bindings");
    if (!bindings || !bindings->get<Value::Object>())
        return std::unexpected(bad("Preset has no bindings"));

    Preset preset;
    preset.name = *name->get<std::string>();
    for (const auto& [id, value] : *bindings->get<Value::Object>()) {
        if (!actions.find(id)) continue;
        const auto* entry = value.get<Value::Object>();
        if (!entry) return std::unexpected(bad("Binding is not an object: " + id));
        Binding binding;
        if (const auto* context = field(*entry, "context"); context && context->get<std::string>()) {
            const auto& contextText = *context->get<std::string>();
            if (contextText == "select_tool") binding.context = BindingContext::SelectToolActive;
            else if (contextText.starts_with("tool:") && contextText.size() > 5)
                binding.context = BindingContext::forTool(contextText.substr(5));
            else if (contextText != "always") return std::unexpected(bad("Unknown context for " + id));
        }
        if (const auto* provisional = field(*entry, "provisional"); provisional && provisional->get<bool>())
            binding.provisional = *provisional->get<bool>();
        if (const auto* reason = field(*entry, "unbound"); reason && reason->get<std::string>()) {
            binding.value = Unbound{*reason->get<std::string>()};
        } else {
            const auto* keys = field(*entry, "keys");
            if (!keys || !keys->get<Value::Array>()) return std::unexpected(bad("Binding has no keys: " + id));
            std::vector<KeyChord> chords;
            for (const auto& key : *keys->get<Value::Array>()) {
                if (!key.get<std::string>()) return std::unexpected(bad("Key is not a string: " + id));
                auto chord = KeyChord::parse(*key.get<std::string>());
                if (!chord) return std::unexpected(bad("Invalid key '" + *key.get<std::string>() + "' for " + id));
                chords.push_back(std::move(*chord));
            }
            if (chords.empty()) binding.value = Unbound{"Unbound by the user."};
            else binding.value = std::move(chords);
        }
        preset.bindings.emplace(id, std::move(binding));
    }
    for (const auto& action : actions.entries()) {
        if (!preset.bindings.contains(action.id))
            preset.bindings.emplace(action.id, Binding{Unbound{"Not set in this preset."}});
    }
    if (const auto problems = validate(preset, actions); !problems.empty()) {
        std::string message = problems.front();
        for (std::size_t i = 1; i < problems.size(); ++i) message += "; " + problems[i];
        return std::unexpected(bad(message));
    }
    return preset;
}

bool isBuiltinPresetName(std::string_view name) {
    const auto wanted = lower(name);
    return std::ranges::any_of(presetNames(), [&](std::string_view builtin) { return lower(builtin) == wanted; });
}

Result<std::string> checkPresetName(std::string_view name) {
    const auto invalid = [](std::string message) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
    };
    std::string trimmed(name);
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    if (trimmed.empty() || trimmed.size() > 64) return invalid("Use 1 to 64 characters.");
    if (trimmed.front() == ' ' || trimmed.front() == '.') return invalid("A name cannot start with a space or a dot.");
    for (unsigned char c : trimmed) {
        if (!(std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.'))
            return invalid("Use letters, digits, spaces, '-', '_' and '.'.");
    }
    if (isBuiltinPresetName(trimmed)) return invalid("That name belongs to a built-in preset.");
    return trimmed;
}

std::optional<ActionId> chordOwner(const Preset& preset, const KeyChord& chord,
                                   std::string_view except, BindingContext context) {
    for (const auto& [id, binding] : preset.bindings) {
        if (id == except || binding.context != context) continue;
        if (const auto* keys = std::get_if<std::vector<KeyChord>>(&binding.value);
            keys && std::ranges::find(*keys, chord) != keys->end())
            return id;
    }
    return std::nullopt;
}

bool allowsHeldModifier(const ActionId& id) { return id == "snap.suspend"; }

const Binding& defaultBinding(const ActionId& id) { return hammerPreset().bindings.at(id); }

void unbindAction(Preset& preset, const ActionId& id, std::string reason) {
    auto& binding = preset.bindings[id];
    binding.value = Unbound{std::move(reason)};
    binding.provisional = false;
}

std::vector<ActionId> assignChords(Preset& preset, const ActionId& id, std::vector<KeyChord> chords) {
    std::vector<ActionId> changed;
    const auto targetContext = preset.bindings.at(id).context;
    for (const auto& chord : chords) {
        for (auto& [other, binding] : preset.bindings) {
            if (other == id || binding.context != targetContext) continue;
            auto* keys = std::get_if<std::vector<KeyChord>>(&binding.value);
            if (!keys) continue;
            const auto before = keys->size();
            std::erase(*keys, chord);
            if (keys->size() == before) continue;
            if (keys->empty()) binding.value = Unbound{"Its key was given to another action."};
            if (std::ranges::find(changed, other) == changed.end()) changed.push_back(other);
        }
    }
    auto& binding = preset.bindings[id];
    if (chords.empty()) {
        unbindAction(preset, id);
        return changed;
    }
    // A rebind takes the action's usual context so W/E/R and the arrows stay tool-scoped.
    if (std::holds_alternative<Unbound>(binding.value)) binding.context = defaultBinding(id).context;
    binding.value = std::move(chords);
    binding.provisional = false;
    return changed;
}

std::filesystem::path PresetStore::pathFor(std::string_view name) const {
    return directory_ / (std::string(name) + ".json");
}

std::vector<std::string> PresetStore::list() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
        if (entry.path().extension() != ".json") continue;
        const auto stem = entry.path().stem().string();
        if (checkPresetName(stem) && load(stem)) names.push_back(stem);
    }
    std::ranges::sort(names);
    return names;
}

Result<Preset> PresetStore::load(std::string_view name) const {
    if (!checkPresetName(name)) return std::unexpected(Error{ErrorCode::NotFound, "No such preset: " + std::string(name)});
    std::ifstream in(pathFor(name), std::ios::binary);
    if (!in) return std::unexpected(Error{ErrorCode::NotFound, "No such preset: " + std::string(name)});
    std::ostringstream text;
    text << in.rdbuf();
    auto preset = parsePreset(text.str());
    if (!preset) return std::unexpected(preset.error().withContext(std::string(name)));
    preset->name = std::string(name); // The file name is the identity.
    return preset;
}

Result<void> PresetStore::save(const Preset& preset) const {
    const auto name = checkPresetName(preset.name);
    if (!name) return std::unexpected(name.error());
    if (const auto problems = validate(preset); !problems.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, problems.front()});
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) return std::unexpected(Error{ErrorCode::IoError, "Cannot create " + directory_.string() + ": " + ec.message()});
    const auto target = pathFor(*name);
    auto temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << serializePreset(preset);
        if (!out) return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + temp.string()});
    }
    std::filesystem::rename(temp, target, ec);
    if (ec) return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + target.string() + ": " + ec.message()});
    return {};
}

Result<void> PresetStore::remove(std::string_view name) const {
    if (!checkPresetName(name)) return std::unexpected(Error{ErrorCode::InvalidArgument, "Not a user preset"});
    std::error_code ec;
    if (!std::filesystem::remove(pathFor(name), ec) || ec)
        return std::unexpected(Error{ErrorCode::IoError, "Cannot delete preset " + std::string(name)});
    return {};
}

Result<Preset> PresetStore::find(std::string_view name) const {
    if (const auto* builtin = findPreset(name)) return *builtin;
    return load(name);
}

} // namespace apex::editor
