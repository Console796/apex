#include "document/instance_fixup.hpp"

#include "core/strings.hpp"
#include "document/connection_codec.hpp"

#include <algorithm>
#include <cctype>

namespace apex::document {
namespace {
std::string text(const kv1::Node& node, std::string_view key) {
    const auto* value = node.find(key);
    return value ? value->decodedValue().value_or("") : "";
}
bool replacementKey(std::string_view key) {
    return key.size() > 7 && ciEquals(key.substr(0, 7), "replace") &&
        std::ranges::all_of(key.substr(7), [](unsigned char c) { return std::isdigit(c); });
}
bool identifier(char c, bool first) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalpha(u) || c == '_' || (!first && std::isdigit(u));
}
}

InstanceKeyType defaultInstanceKeyType(std::string_view, std::string_view key) {
    if (ciEquals(key, "origin")) return InstanceKeyType::Position;
    if (ciEquals(key, "angles")) return InstanceKeyType::Angles;
    if (ciEquals(key, "pitch")) return InstanceKeyType::Pitch;
    if (ciEquals(key, "yaw")) return InstanceKeyType::Yaw;
    for (const auto* name : {"targetname", "target", "parentname", "filtername", "killtarget",
                              "landmark", "master", "measuretarget", "measurereference",
                              "targetreference", "lightingorigin"})
        if (ciEquals(key, name)) return InstanceKeyType::Name;
    return InstanceKeyType::String;
}

InstanceFixup InstanceFixup::parse(const kv1::Node& instance) {
    InstanceFixup result;
    result.name = text(instance, "targetname");
    if (const auto* value = instance.find("fixup_style")) {
        const int style = value->asInt().value_or(0);
        if (style == 1) result.style = FixupStyle::Postfix;
        if (style == 2) result.style = FixupStyle::None;
    }
    for (const auto& value : instance.children()) {
        const auto key = value.key();
        if (value.kind() != kv1::NodeKind::Value || !replacementKey(key)) continue;
        const auto raw = value.decodedValue().value_or("");
        const auto split = raw.find(' ');
        std::string name = raw.substr(0, split);
        while (name.starts_with('$')) name.erase(0, 1);
        if (name.empty()) continue;
        const auto replacement = split == std::string::npos ? "" : raw.substr(split + 1);
        auto found = std::ranges::find_if(result.variables, [&](const auto& var) { return ciEquals(var.first, name); });
        if (found == result.variables.end()) result.variables.emplace_back(name, replacement);
        else found->second = replacement;
    }
    std::ranges::stable_sort(result.variables, [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    return result;
}

std::string InstanceFixup::substitute(std::string_view source) const {
    std::string result;
    // srctools' empty variable matcher consumes the dollar only, even before identifiers.
    if (variables.empty()) {
        for (char c : source) if (c != '$') result += c;
        return result;
    }
    for (std::size_t i = 0; i < source.size();) {
        if (source[i] != '$') { result += source[i++]; continue; }
        const auto rest = source.substr(i + 1);
        const std::pair<std::string, std::string>* longest = nullptr;
        for (const auto& variable : variables) {
            const auto& key = variable.first;
            if (!key.empty() && rest.size() >= key.size() &&
                (!longest || key.size() > longest->first.size()) && ciEquals(rest.substr(0, key.size()), key))
                longest = &variable;
        }
        if (longest) {
            result += longest->second;
            i += longest->first.size() + 1;
            continue;
        }
        if (!rest.empty() && identifier(rest[0], true)) {
            ++i;
            while (i < source.size() && identifier(source[i], false)) ++i;
        } else result += source[i++];
    }
    return result;
}

std::string InstanceFixup::fixName(std::string_view value) const {
    if (value.empty() || value.front() == '@' || value.front() == '!' || style == FixupStyle::None) return std::string(value);
    return style == FixupStyle::Prefix ? name + '-' + std::string(value) : std::string(value) + '-' + name;
}

kv1::Node fixedInstanceKeys(const kv1::Node& entity, const InstanceFixup& fixup, const InstanceKeyTypes& types,
                            const std::function<bool(std::string_view)>& isClassname) {
    auto result = entity;
    const auto classname = text(entity, "classname");
    for (auto& child : result.children()) {
        if (child.kind() == kv1::NodeKind::Value) {
            const auto raw = child.decodedValue().value_or("");
            const bool replacement = ciEquals(classname, "func_instance") && replacementKey(child.key());
            const auto space = raw.find(' ');
            auto value = replacement ? (space == std::string::npos ? raw :
                raw.substr(0, space + 1) + fixup.substitute(raw.substr(space + 1))) : fixup.substitute(raw);
            const auto type = types ? types(classname, child.key()) : defaultInstanceKeyType(classname, child.key());
            if (type == InstanceKeyType::Name ||
                (type == InstanceKeyType::NameOrClass && (!isClassname || !isClassname(value))))
                value = fixup.fixName(value);
            // VBSP guesses that nonnumeric nested replacement values are names.
            if (ciEquals(classname, "func_instance") && replacementKey(child.key())) {
                const auto split = value.find(' ');
                if (split != std::string::npos && split + 1 < value.size() &&
                    std::string_view("@!-.0123456789").find(value[split + 1]) == std::string_view::npos)
                    value = value.substr(0, split + 1) + fixup.fixName(value.substr(split + 1));
            }
            (void)child.setValue(std::move(value));
        } else if (ciEquals(child.key(), "connections")) {
            for (auto& row : child.children()) {
                if (row.kind() != kv1::NodeKind::Value) continue;
                auto connection = parseConnection(row.decodedValue().value_or(""));
                connection.target = fixup.fixName(fixup.substitute(connection.target));
                connection.input = fixup.substitute(connection.input);
                connection.parameter = fixup.substitute(connection.parameter);
                (void)row.setValue(serialize(connection));
            }
        } else if (ciEquals(child.key(), "solid") || ciEquals(child.key(), "side")) {
            child = fixedInstanceKeys(child, fixup, [](auto, auto) { return InstanceKeyType::String; });
        }
    }
    return result;
}

} // namespace apex::document
