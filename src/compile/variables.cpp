#include "compile/variables.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>

namespace apex::compile {
namespace {
Result<std::string> variable(std::string_view text, std::size_t& pos, const Variables& variables) {
    const auto start = ++pos;
    while (pos < text.size() && (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) ++pos;
    const auto name = text.substr(start, pos - start);
    constexpr std::array names{"gamedir", "path", "file", "bspdir", "bsp_exe", "vis_exe", "light_exe",
                               "game_exe", "postcompiler_exe", "addon", "mapname", "work"};
    const auto it = variables.find(name);
    if (std::ranges::find(names, name) == names.end() || it == variables.end()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Unknown compile variable $" + std::string(name)});
    }
    return it->second;
}
char separator(char c) {
    return c == '\\' ? static_cast<char>(std::filesystem::path::preferred_separator) : c;
}
}

Result<std::string> expandVariables(std::string_view text, const Variables& variables) {
    std::string result;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '$') {
            auto value = variable(text, i, variables);
            if (!value) return std::unexpected(value.error());
            result += *value;
        } else result += separator(text[i++]);
    }
    return result;
}

Result<std::vector<std::string>> expandArguments(std::string_view text, const Variables& variables) {
    std::vector<std::string> result;
    std::string token;
    bool quoted = false, started = false;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '$') {
            auto value = variable(text, i, variables);
            if (!value) return std::unexpected(value.error());
            token += *value;
            started = true;
        } else if (c == '"') {
            quoted = !quoted;
            started = true;
            ++i;
        } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (started) result.push_back(std::move(token));
            token.clear();
            started = false;
            ++i;
        } else {
            token += separator(c);
            started = true;
            ++i;
        }
    }
    if (quoted) return std::unexpected(Error{ErrorCode::ParseError, "Unclosed quote in compile arguments"});
    if (started) result.push_back(std::move(token));
    return result;
}

} // namespace apex::compile
