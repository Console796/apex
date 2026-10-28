#include "editor/connection_completion.hpp"

#include <algorithm>

namespace apex::editor {
namespace {
std::vector<std::string> sorted(std::vector<std::string> values) {
    std::ranges::sort(values, [](const auto& a, const auto& b) {
        const auto lowerA = toLowerAscii(a), lowerB = toLowerAscii(b);
        return lowerA == lowerB ? a < b : lowerA < lowerB;
    });
    values.erase(std::unique(values.begin(), values.end(), [](const auto& a, const auto& b) {
        return ciEquals(a, b);
    }), values.end());
    return values;
}
}
std::vector<std::string> outputsFor(const assets::EntityClassTable& classes, std::string_view classname) {
    std::vector<std::string> result;
    if (const auto* cls = classes.find(classname))
        for (const auto& io : cls->outputs) result.push_back(io.name);
    return sorted(std::move(result));
}
std::vector<std::string> targetsFor(const NameIndex& names, std::string_view prefix) {
    auto result = names.names();
    for (auto& name : specialTargets()) result.push_back(std::move(name));
    if (prefix.find('*') != std::string_view::npos) result.emplace_back(prefix);
    else std::erase_if(result, [&](const auto& name) { return !toLowerAscii(name).starts_with(toLowerAscii(prefix)); });
    return sorted(std::move(result));
}
std::vector<std::string> inputsFor(const ConnectionValidator& context, std::string_view targetText) {
    std::vector<std::string> result;
    for (auto id : context.matching(targetText)) {
        if (const auto* cls = context.entityClass(id))
            for (const auto& io : cls->inputs) result.push_back(io.name);
        auto instanceEntries = context.instanceInputs(id);
        result.insert(result.end(), std::make_move_iterator(instanceEntries.begin()),
                      std::make_move_iterator(instanceEntries.end()));
    }
    return sorted(std::move(result));
}
std::vector<std::string> parameterChoices(const assets::EntityClassTable& classes,
    std::string_view inputType, const NameIndex& names) {
    (void)classes;
    if (ciEquals(inputType, "bool")) return {"0", "1"};
    if (toLowerAscii(inputType).starts_with("target")) return targetsFor(names);
    return {};
}

} // namespace apex::editor
