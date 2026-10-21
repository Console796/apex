#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace apex {

// ASCII only: bytes outside A-Z are preserved, independent of the process locale.
std::string toLowerAscii(std::string_view text);
bool ciEquals(std::string_view a, std::string_view b);
bool ciLess(std::string_view a, std::string_view b);

struct CiHash {
    std::size_t operator()(std::string_view text) const noexcept;
};

// Views borrow the input. Split preserves empty fields (including one for empty input).
std::vector<std::string_view> split(std::string_view text, char delimiter);
std::string_view trim(std::string_view text);
bool startsWith(std::string_view text, std::string_view prefix);

} // namespace apex
