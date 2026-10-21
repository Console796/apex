#pragma once

#include "core/kv1.hpp"

namespace apex::kv1::detail {

inline bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

inline bool conditionStart(std::string_view text, std::size_t offset) {
    return text.substr(offset, 2) == "[$" || text.substr(offset, 3) == "[!$";
}

inline bool validCondition(std::string_view text) {
    if (text.empty()) {
        return true;
    }
    if (!conditionStart(text, 0) || text.back() != ']') {
        return false;
    }
    const auto begin = text[1] == '!' ? 3U : 2U;
    if (text.size() <= begin + 1) {
        return false;
    }
    for (std::size_t i = begin; i + 1 < text.size(); ++i) {
        const char c = text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

inline Error errorAt(std::string_view source, SourceSpan span, std::string message) {
    const auto previous = span.offset == 0 ? std::string_view::npos
                                          : source.rfind('\n', span.offset - 1);
    const auto column = previous == std::string_view::npos ? span.offset + 1
                                                          : span.offset - previous;
    return Error{ErrorCode::ParseError, std::move(message)}.withContext(
        "line " + std::to_string(span.line) + ", column " + std::to_string(column));
}

inline std::string decode(std::string_view raw, EscapeMode mode) {
    std::string result;
    result.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (mode == EscapeMode::Strata && raw[i] == '\\' && i + 1 < raw.size()) {
            switch (raw[i + 1]) {
            case 'n': result += '\n'; ++i; continue;
            case 't': result += '\t'; ++i; continue;
            case '"': result += '"'; ++i; continue;
            case '\\': result += '\\'; ++i; continue;
            default: break; // Unknown escapes retain their backslash.
            }
        }
        result += raw[i];
    }
    return result;
}

} // namespace apex::kv1::detail
