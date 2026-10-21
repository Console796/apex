#pragma once

#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace apex {

enum class ErrorCode { InvalidArgument, NotFound, ParseError, IoError, Unsupported, Internal };

struct Error {
    ErrorCode code;
    std::string message;
    std::vector<std::string> context{}; // Innermost first; withContext adds an outer frame.

    [[nodiscard]] Error withContext(std::string outer) const {
        Error result = *this;
        result.context.push_back(std::move(outer));
        return result;
    }

    std::string what() const {
        std::string result;
        for (auto it = context.rbegin(); it != context.rend(); ++it) {
            result += *it;
            result += ": ";
        }
        return result + message;
    }
};

template <typename T> using Result = std::expected<T, Error>;

} // namespace apex
