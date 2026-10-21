#include "core/strings.hpp"

#include <algorithm>
#include <cstdint>

namespace apex {
namespace {

unsigned char lowerAscii(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

} // namespace

std::string toLowerAscii(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = static_cast<char>(lowerAscii(static_cast<unsigned char>(c)));
    }
    return result;
}

bool ciEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return lowerAscii(static_cast<unsigned char>(x)) ==
               lowerAscii(static_cast<unsigned char>(y));
    });
}

bool ciLess(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return lowerAscii(static_cast<unsigned char>(x)) <
               lowerAscii(static_cast<unsigned char>(y));
    });
}

std::size_t CiHash::operator()(std::string_view text) const noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : text) {
        hash ^= lowerAscii(c);
        hash *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(hash);
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        const auto end = text.find(delimiter, start);
        parts.push_back(text.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool startsWith(std::string_view text, std::string_view prefix) { return text.starts_with(prefix); }

} // namespace apex
