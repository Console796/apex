#include "core/json.hpp"

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <type_traits>

namespace apex::json {
namespace {

class Parser {
    std::string_view source_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail() const {
        throw std::runtime_error("Invalid JSON at byte " + std::to_string(pos_));
    }
    void space() {
        while (pos_ < source_.size() && (source_[pos_] == ' ' || source_[pos_] == '\t' ||
               source_[pos_] == '\r' || source_[pos_] == '\n')) {
            ++pos_;
        }
    }
    bool take(char c) {
        space();
        if (pos_ == source_.size() || source_[pos_] != c) {
            return false;
        }
        ++pos_;
        return true;
    }
    unsigned hex() {
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ == source_.size()) {
                fail();
            }
            char c = source_[pos_++];
            unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                             c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                             c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit == 16) {
                fail();
            }
            result = result * 16 + digit;
        }
        return result;
    }
    std::string string() {
        if (!take('"')) {
            fail();
        }
        std::string result;
        while (pos_ < source_.size()) {
            unsigned char c = static_cast<unsigned char>(source_[pos_++]);
            if (c == '"') {
                return result;
            }
            if (c < 32) {
                fail();
            }
            if (c != '\\') {
                result += static_cast<char>(c);
                continue;
            }
            if (pos_ == source_.size()) {
                fail();
            }
            switch (source_[pos_++]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                unsigned cp = hex();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (pos_ + 2 > source_.size() || source_.substr(pos_, 2) != "\\u") {
                        fail();
                    }
                    pos_ += 2;
                    const unsigned low = hex();
                    if (low < 0xdc00 || low > 0xdfff) {
                        fail();
                    }
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    fail();
                }
                if (cp < 0x80) {
                    result += static_cast<char>(cp);
                } else {
                    if (cp >= 0x10000) {
                        result += static_cast<char>(0xf0 | (cp >> 18));
                    }
                    if (cp >= 0x800) {
                        result += static_cast<char>((cp >= 0x10000 ? 0x80 : 0xe0) |
                                                    ((cp >> 12) & 63));
                    }
                    result += static_cast<char>((cp >= 0x800 ? 0x80 : 0xc0) | ((cp >> 6) & 63));
                    result += static_cast<char>(0x80 | (cp & 63));
                }
                break;
            }
            default: fail();
            }
        }
        fail();
    }
    Value value(unsigned depth) {
        if (depth > 128) {
            fail();
        }
        space();
        if (pos_ == source_.size()) {
            fail();
        }
        if (source_[pos_] == '"') {
            return {string()};
        }
        if (take('{')) {
            Value::Object object;
            if (take('}')) {
                return {object};
            }
            do {
                auto key = string();
                if (!take(':')) {
                    fail();
                }
                auto child = value(depth + 1);
                if (!object.emplace(std::move(key), std::move(child)).second) {
                    fail();
                }
                if (take('}')) {
                    return {object};
                }
            } while (take(','));
            fail();
        }
        if (take('[')) {
            Value::Array array;
            if (take(']')) {
                return {array};
            }
            do {
                array.push_back(value(depth + 1));
                if (take(']')) {
                    return {array};
                }
            } while (take(','));
            fail();
        }
        for (auto word : {std::string_view("true"), std::string_view("false"),
                          std::string_view("null")}) {
            if (source_.substr(pos_, word.size()) == word) {
                pos_ += word.size();
                if (word == "null") {
                    return {};
                }
                return {word == "true"};
            }
        }
        const auto start = pos_;
        if (source_[pos_] == '-') {
            ++pos_;
        }
        auto digits = [&] {
            const auto before = pos_;
            while (pos_ < source_.size() && source_[pos_] >= '0' && source_[pos_] <= '9') {
                ++pos_;
            }
            if (before == pos_) {
                fail();
            }
        };
        if (pos_ < source_.size() && source_[pos_] == '0') {
            ++pos_;
        } else {
            digits();
        }
        if (pos_ < source_.size() && source_[pos_] == '.') {
            ++pos_;
            digits();
        }
        if (pos_ < source_.size() && (source_[pos_] == 'e' || source_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) {
                ++pos_;
            }
            digits();
        }
        double number = 0;
        auto converted = std::from_chars(source_.data() + start, source_.data() + pos_, number);
        if (converted.ec != std::errc{} || !std::isfinite(number)) {
            fail();
        }
        return {number};
    }

public:
    explicit Parser(std::string_view source) : source_(source) {}
    Value parse() {
        auto result = value(0);
        space();
        if (pos_ != source_.size()) {
            fail();
        }
        return result;
    }
};

std::string quote(std::string_view text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}

} // namespace

Result<Value> parse(std::string_view source) {
    try {
        return Parser(source).parse();
    } catch (const std::runtime_error& e) {
        return std::unexpected(Error{ErrorCode::ParseError, e.what()});
    }
}

std::string write(const Value& value) {
    return std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
            return item ? "true" : "false";
        } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(item)) {
                throw std::invalid_argument("Non-finite JSON number");
            }
            char buffer[64];
            auto result = std::to_chars(buffer, buffer + sizeof(buffer), item);
            return {buffer, result.ptr};
        } else if constexpr (std::is_same_v<T, std::string>) {
            return quote(item);
        } else {
            constexpr bool object = std::is_same_v<T, Value::Object>;
            std::string out = object ? "{" : "[";
            bool first = true;
            for (const auto& child : item) {
                if (!first) {
                    out += ',';
                }
                first = false;
                if constexpr (object) {
                    out += quote(child.first) + ':' + write(child.second);
                } else {
                    out += write(child);
                }
            }
            return out + (object ? '}' : ']');
        }
    }, value.data);
}

} // namespace apex::json
