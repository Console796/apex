#include "assets/fgd.hpp"

#include "core/strings.hpp"

#include <algorithm>
#include <exception>
#include <functional>
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
} // namespace
const EntityClass* EntityClassTable::find(std::string_view name) const {
    auto it = classes.find(toLowerAscii(name));
    return it == classes.end() ? nullptr : &it->second;
}
Result<EntityClassTable> loadEntityClasses(std::span<const std::filesystem::path> files) try {
    toolpp::FGD parsed;
    for (const auto& file : files) {
        if (!std::filesystem::is_regular_file(file))
            return std::unexpected(Error{ErrorCode::NotFound, "FGD missing: " + file.string()});
        parsed.load(file);
    }
    EntityClassTable result;
    for (const auto& [name, entity] : parsed.getEntities()) {
        EntityClass out;
        out.name = name;
        out.type = entity.classType;
        for (const auto& prop : entity.classProperties) {
            auto key = toLowerAscii(trim(prop.name));
            if (key == "base") {
                for (auto base : split(prop.arguments, ',')) {
                    out.bases.push_back(unquote(base));
                }
            } else if (key == "color") {
                out.color = vector3<int>(prop.arguments);
            } else if (key == "size") {
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
