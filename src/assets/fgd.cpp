#include "assets/fgd.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <fstream>
#include <set>
#include <iterator>
#include <sstream>
#include <toolpp/FGD.h>

namespace apex::assets {
namespace {
template <typename T> std::optional<std::array<T, 3>> vector3(std::string_view text) {
    std::istringstream stream{std::string(text)};
    std::array<T, 3> result;
    if (!(stream >> result[0] >> result[1] >> result[2])) {
        return {};
    }
    return result;
}
std::string unquote(std::string_view text) {
    text = trim(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        text = text.substr(1, text.size() - 2);
    return std::string(text);
}
// Recover source ordering and header spans; toolpp interprets field metadata.
struct OrderToken {
    std::string text;
    bool quoted = false;
    std::size_t begin = 0, end = 0;
};
std::vector<OrderToken> orderTokens(std::string_view text) {
    std::vector<OrderToken> result;
    for (std::size_t i = 0; i < text.size();) {
        if (std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        if (text.substr(i, 2) == "//") {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        const auto begin = i;
        if (text[i] == '"') {
            std::string value;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                value += text[i];
            }
            if (i < text.size()) ++i;
            result.push_back({std::move(value), true, begin, i});
        } else if (std::string_view("[]()=:,+").find(text[i]) != std::string_view::npos) {
            ++i;
            result.push_back({std::string(text.substr(begin, 1)), false, begin, i});
        } else {
            auto start = i++;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
                   std::string_view("[]()=:,+\"").find(text[i]) == std::string_view::npos) ++i;
            result.push_back({std::string(text.substr(start, i - start)), false, begin, i});
        }
    }
    return result;
}
using PropertyOrders = std::map<std::string, std::vector<std::string>>;
using ClassHeaders = std::map<std::string, std::vector<EntityHelper>>;
// Toolpp stops at the first ')' even inside quotes. Recover headers and flatten includes,
// then pass a copy without class properties to toolpp for field metadata parsing.
std::string readOrder(const std::filesystem::path& file, PropertyOrders& orders,
                      ClassHeaders& headers, std::set<std::filesystem::path>& seen) {
    if (!seen.insert(std::filesystem::weakly_canonical(file)).second) return {};
    std::ifstream input(file);
    if (!input) throw std::runtime_error("FGD missing: " + file.string());
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    const auto tokens = orderTokens(text);
    int brackets = 0, parens = 0;
    std::string entity;
    bool classHeader = false;
    std::size_t headerStart = 0;
    struct Replacement { std::size_t begin, end; std::string text; };
    std::vector<Replacement> replacements;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (token.quoted) continue;
        const auto key = toLowerAscii(token.text);
        if (brackets == 0 && key == "@include" && i + 1 < tokens.size()) {
            const auto begin = token.begin;
            const auto& path = tokens[++i];
            auto included = readOrder(file.parent_path() / path.text, orders, headers, seen);
            replacements.push_back({begin, path.end, "\n" + included + "\n"});
        } else if (brackets == 0 && key.starts_with('@')) {
            classHeader = key.ends_with("class");
            headerStart = i;
            entity.clear();
        } else if (brackets == 0 && parens == 0 && key == "=" && classHeader && i + 1 < tokens.size()) {
            auto& header = headers[toLowerAscii(tokens[i + 1].text)];
            header.clear(); // Like toolpp, ExtendClass replaces the class header.
            for (std::size_t j = headerStart + 1; j < i; ++j) {
                EntityHelper helper{toLowerAscii(tokens[j].text), {}};
                if (j + 1 < i && tokens[j + 1].text == "(") {
                    const auto start = tokens[++j].end;
                    unsigned depth = 1;
                    while (++j < i) {
                        if (tokens[j].quoted) continue;
                        if (tokens[j].text == "(") ++depth;
                        else if (tokens[j].text == ")" && --depth == 0) break;
                    }
                    if (j < i) helper.arguments = text.substr(start, tokens[j].begin - start);
                }
                header.push_back(std::move(helper));
            }
            replacements.push_back({tokens[headerStart].end, token.begin, " "});
            entity = toLowerAscii(tokens[++i].text);
        } else if (key == "[") ++brackets;
        else if (key == "]") --brackets;
        else if (key == "(") ++parens;
        else if (key == ")") --parens;
        else if (!entity.empty() && brackets == 1 && parens == 0 && i + 1 < tokens.size() &&
                 tokens[i + 1].text == "(" &&
                 (i == 0 || (tokens[i - 1].text != "input" && tokens[i - 1].text != "output"))) {
            auto& order = orders[entity];
            if (std::ranges::find(order, key) == order.end()) order.push_back(key);
        }
    }
    std::string sanitized = text;
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it)
        sanitized.replace(it->begin, it->end - it->begin, it->text);
    return sanitized;
}
class HeaderlessFgd : public toolpp::FGD {
public:
    void loadText(const std::string& text, const std::filesystem::path& file) {
        BufferStreamReadOnly stream{text.data(), text.size()};
        std::vector seen{file};
        try { readEntities(stream, file, seen); }
        catch (const std::overflow_error&) {} // Toolpp uses stream overflow for EOF.
    }
};
template <typename T> void mergeFields(std::vector<T>& into, const std::vector<T>& from, bool replace) {
    for (const auto& field : from) {
        auto it = std::ranges::find_if(into, [&](const auto& old) { return ciEquals(old.name, field.name); });
        if (it == into.end()) into.push_back(field);
        else if (replace) *it = field;
    }
}
} // namespace
Result<EntityClassTable> loadEntityClasses(std::span<const std::filesystem::path> files) try {
    HeaderlessFgd parsed;
    PropertyOrders orders;
    ClassHeaders headers;
    for (const auto& file : files) {
        if (!std::filesystem::is_regular_file(file))
            return std::unexpected(Error{ErrorCode::NotFound, "FGD missing: " + file.string()});
        std::set<std::filesystem::path> seen;
        parsed.loadText(readOrder(file, orders, headers, seen), file);
    }
    EntityClassTable result;
    for (const auto& [name, entity] : parsed.getEntities()) {
        EntityClass out;
        out.name = name;
        out.type = entity.classType;
        for (const auto& prop : headers[toLowerAscii(name)]) {
            auto key = toLowerAscii(trim(prop.name));
            if (key != "base") out.helpers.push_back({key, prop.arguments});
            if (key == "base") {
                for (auto base : split(prop.arguments, ',')) {
                    out.bases.push_back(unquote(base));
                }
            } else if (key == "color") {
                out.color = vector3<int>(prop.arguments);
            } else if (key == "size" || key == "bbox") {
                auto halves = split(prop.arguments, ',');
                if (halves.size() == 2) {
                    out.mins = vector3<double>(halves[0]);
                    out.maxs = vector3<double>(halves[1]);
                } else if (halves.size() == 1) {
                    auto size = vector3<double>(halves[0]);
                    if (size) {
                        out.mins = *size;
                        out.maxs = *size;
                        for (unsigned i = 0; i < 3; ++i) {
                            (*out.mins)[i] = -(*size)[i] / 2;
                            (*out.maxs)[i] = (*size)[i] / 2;
                        }
                    }
                }
            } else if (key == "iconsprite") {
                out.iconSprite = unquote(prop.arguments);
            }
        }
        for (const auto& field : entity.fields) {
            out.properties.push_back({std::string(field.name), std::string(field.valueType),
                std::string(field.displayName), std::string(field.valueDefault),
                std::string(field.description), field.readonly, {}, {}});
        }
        for (const auto& field : entity.fieldsWithChoices) {
            EntityProperty prop{std::string(field.name), "choices", std::string(field.displayName),
                std::string(field.valueDefault), std::string(field.description), field.readonly, {}, {}};
            for (const auto& choice : field.choices)
                prop.choices.push_back({std::string(choice.value), std::string(choice.displayName)});
            out.properties.push_back(std::move(prop));
        }
        for (const auto& field : entity.fieldsWithFlags) {
            EntityProperty prop{std::string(field.name), "flags", std::string(field.displayName),
                "", std::string(field.description), field.readonly, {}, {}};
            std::uint64_t defaults = 0;
            for (const auto& flag : field.flags) {
                prop.flags.push_back({flag.value, std::string(flag.displayName), flag.enabledByDefault});
                if (flag.enabledByDefault) defaults |= flag.value;
            }
            prop.defaultValue = std::to_string(defaults);
            out.properties.push_back(std::move(prop));
        }
        const auto& order = orders[toLowerAscii(name)];
        const auto position = [&](const EntityProperty& prop) {
            return std::ranges::find(order, toLowerAscii(prop.name)) - order.begin();
        };
        std::ranges::stable_sort(out.properties, {}, position);
        for (const auto& io : entity.inputs)
            out.inputs.push_back({std::string(io.name), std::string(io.valueType), std::string(io.description)});
        for (const auto& io : entity.outputs)
            out.outputs.push_back({std::string(io.name), std::string(io.valueType), std::string(io.description)});
        result.classes[toLowerAscii(name)] = std::move(out);
    }
    std::map<std::string, unsigned> states;
    std::function<Result<void>(const std::string&, unsigned)> resolve =
        [&](const std::string& key, unsigned depth) -> Result<void> {
        auto& state = states[key];
        if (state == 2) {
            return {};
        }
        if (state == 1 || depth > 128) {
            return std::unexpected(Error{ErrorCode::ParseError, "FGD base cycle/depth: " + key});
        }
        state = 1;
        auto& entity = result.classes.at(key);
        std::vector<EntityHelper> helpers;
        std::vector<EntityProperty> properties;
        std::vector<EntityIO> inputs, outputs;
        for (const auto& baseName : entity.bases) {
            auto base = result.classes.find(toLowerAscii(baseName));
            if (base == result.classes.end()) {
                result.unresolvedBases.push_back(entity.name + ": " + baseName);
                continue;
            }
            auto resolved = resolve(base->first, depth + 1);
            if (!resolved) {
                return resolved;
            }
            helpers.insert(helpers.end(), base->second.helpers.begin(), base->second.helpers.end());
            mergeFields(properties, base->second.properties, false);
            mergeFields(inputs, base->second.inputs, false);
            mergeFields(outputs, base->second.outputs, false);
            if (!entity.color) {
                entity.color = base->second.color;
            }
            if (!entity.mins) {
                entity.mins = base->second.mins;
            }
            if (!entity.maxs) {
                entity.maxs = base->second.maxs;
            }
            if (entity.iconSprite.empty()) {
                entity.iconSprite = base->second.iconSprite;
            }
        }
        mergeFields(properties, entity.properties, true);
        mergeFields(inputs, entity.inputs, true);
        mergeFields(outputs, entity.outputs, true);
        helpers.insert(helpers.end(), entity.helpers.begin(), entity.helpers.end());
        entity.helpers = std::move(helpers);
        entity.properties = std::move(properties);
        entity.inputs = std::move(inputs);
        entity.outputs = std::move(outputs);
        state = 2;
        return {};
    };
    for (const auto& [key, entity] : result.classes) {
        auto resolved = resolve(key, 0);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
    }
    std::ranges::sort(result.unresolvedBases);
    auto duplicates = std::ranges::unique(result.unresolvedBases);
    result.unresolvedBases.erase(duplicates.begin(), duplicates.end());
    return result;
} catch (const std::exception& e) {
    return std::unexpected(Error{ErrorCode::ParseError, e.what()});
}
Result<EntityClassTable> loadEntityClasses(const std::filesystem::path& file) {
    return loadEntityClasses(std::span(&file, 1));
}
} // namespace apex::assets
