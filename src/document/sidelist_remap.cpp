#include "document/sidelist_remap.hpp"

#include "core/strings.hpp"

namespace apex::document {

std::vector<std::pair<std::string, std::string>> remapSideLists(kv1::Node& entity, const InstanceKeyTypes& types,
                                                                const std::map<int, int>& sides,
                                                                UnmappedSide unmapped) {
    std::vector<std::pair<std::string, std::string>> changed;
    if (!types) return changed;
    const auto* classname = entity.find("classname");
    const std::string cls = classname ? classname->decodedValue().value_or("") : "";
    for (auto& value : entity.children()) {
        if (value.kind() != kv1::NodeKind::Value || types(cls, value.key()) != InstanceKeyType::SideList) continue;
        // Own the text: split() returns views into it.
        const std::string raw = value.decodedValue().value_or("");
        std::string mapped;
        for (const auto word : split(raw, ' ')) {
            if (word.empty()) continue;
            const auto old = kv1::Node::value("", std::string(word)).asInt();
            const auto found = old ? sides.find(*old) : sides.end();
            std::string out;
            if (found != sides.end()) out = std::to_string(found->second);
            else if (unmapped == UnmappedSide::Keep) out = std::string(word);
            else continue;
            if (!mapped.empty()) mapped += ' ';
            mapped += out;
        }
        if (mapped == raw) continue;
        (void)value.setValue(mapped);
        changed.emplace_back(std::string(value.key()), std::move(mapped));
    }
    return changed;
}

} // namespace apex::document
