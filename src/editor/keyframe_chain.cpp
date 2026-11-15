#include "editor/keyframe_chain.hpp"

#include "document/entity_helpers.hpp"

#include <cctype>
#include <set>

namespace apex::editor {
namespace {

struct Keyframe {
    document::NodeHandle handle;
    std::string classname, key, name, next;
};

std::string value(const kv1::Node& block, std::string_view key) {
    const auto* node = block.find(key);
    return node ? node->decodedValue().value_or("") : "";
}

std::optional<Keyframe> keyframe(const document::Document& doc, const assets::EntityClassTable& classes, NodeId id) {
    const auto handle = doc.handle(id);
    if (!handle || !std::holds_alternative<Handle<document::Entity>>(*handle)) return {};
    const auto* node = doc.get(*handle);
    if (!node || !node->block) return {};
    const auto classname = value(*node->block, "classname");
    const auto* cls = classes.find(classname);
    if (!cls || (!document::hasEntityHelper(*cls, "keyframe") && !document::hasEntityHelper(*cls, "animator"))) return {};
    auto key = assets::chainKey(*cls);
    if (key.empty()) return {};
    auto next = value(*node->block, key);
    return Keyframe{*handle, classname, std::move(key), value(*node->block, "targetname"), std::move(next)};
}

// The first free name after `name` counting up its trailing number, keeping its zero padding:
// "rope_3" -> "rope_4", "node09" -> "node10", "wire" -> "wire_2".
std::string following(std::string_view name, std::set<std::string>& taken) {
    auto end = name.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1]))) --end;
    std::string base(name.substr(0, end));
    const auto digits = name.substr(end);
    long long number = 1;
    if (!digits.empty() && digits.size() <= 12) number = std::stoll(std::string(digits));
    else if (!digits.empty()) base = std::string(name) + "_";
    else base += "_";
    for (;;) {
        auto text = std::to_string(++number);
        if (!digits.empty() && digits.size() <= 12 && text.size() < digits.size())
            text.insert(0, digits.size() - text.size(), '0');
        auto candidate = base + text;
        if (taken.insert(toLowerAscii(candidate)).second) return candidate;
    }
}

} // namespace

Result<void> stageKeyframeChain(const document::Document& doc, document::Transaction& tx,
                                const assets::EntityClassTable& classes, std::span<const NodeId> originals,
                                const EditedObjects& copies, std::span<const std::string> names) {
    std::vector<Keyframe> before, after;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        if (auto k = keyframe(doc, classes, id)) before.push_back(std::move(*k));
        const auto handle = doc.handle(id);
        if (const auto* node = handle ? doc.get(*handle) : nullptr)
            for (const auto child : node->children) self(self, child);
    };
    for (const auto id : originals) walk(walk, id);
    for (const auto& item : copies.items)
        if (item.kind == EditedObjects::Item::Kind::Entity)
            if (auto k = keyframe(doc, classes, item.node)) after.push_back(std::move(*k));
    if (after.empty()) return {};

    std::set<std::string> taken;
    for (const auto& name : names) taken.insert(toLowerAscii(name));
    const auto set = [&](document::NodeHandle handle, const std::string& key, std::optional<std::string> text) -> Result<void> {
        try {
            tx.setKeyValue(handle, key, std::move(text));
        } catch (const std::exception& e) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, e.what()});
        }
        return {};
    };

    if (before.size() == 1 && after.size() == 1) {
        const auto& original = before.front();
        const auto& copy = after.front();
        auto name = original.name;
        if (name.empty()) {
            name = original.classname + "_0";
            name = following(name, taken);
            if (auto done = set(original.handle, "targetname", name); !done) return done;
        }
        const auto next = following(name, taken);
        if (auto done = set(copy.handle, "targetname", next); !done) return done;
        if (auto done = set(copy.handle, copy.key, original.next.empty() ? std::nullopt : std::optional(original.next)); !done)
            return done;
        return set(original.handle, original.key, next);
    }

    std::map<std::string, std::string> renamed; // Lower-case original name -> the copy's name.
    for (const auto& copy : after) {
        if (copy.name.empty()) continue;
        const auto name = following(copy.name, taken);
        renamed.try_emplace(toLowerAscii(copy.name), name);
        if (auto done = set(copy.handle, "targetname", name); !done) return done;
    }
    for (const auto& copy : after)
        if (const auto it = renamed.find(toLowerAscii(copy.next)); !copy.next.empty() && it != renamed.end())
            if (auto done = set(copy.handle, copy.key, it->second); !done) return done;
    return {};
}

} // namespace apex::editor
