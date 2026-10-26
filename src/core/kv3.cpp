#include "core/kv3.hpp"

#include <charconv>
#include <cmath>

namespace apex::kv3 {

const Value* Value::find(std::string_view key) const {
    if (auto object = get<Object>())
        for (const auto& member : *object)
            if (member.key == key) return &member.value;
    return nullptr;
}

namespace {
class Parser {
public:
    explicit Parser(std::string_view source) : s(source) {}
    Document run() {
        if (s.starts_with("\xef\xbb\xbf")) p = 3;
        whitespace();
        if (!take("<!--")) fail("Missing KV3 header");
        whitespace();
        if (!take("kv3")) fail("Invalid KV3 header");
        whitespace();
        Document doc;
        doc.encoding = headerField("encoding:");
        if (doc.encoding != "text") fail("Unsupported KV3 encoding: " + doc.encoding + " (text required)");
        whitespace();
        doc.format = headerField("format:");
        whitespace();
        if (!take("-->")) fail("Unterminated KV3 header");
        doc.root = value(0);
        trivia();
        if (p != s.size()) fail("Trailing KV3 input");
        return doc;
    }
private:
    std::string_view s;
    std::size_t p = 0, line = 1;
    [[noreturn]] void fail(std::string message) const {
        throw Error{ErrorCode::ParseError, "line " + std::to_string(line) + ": " + message};
    }
    char peek() const { return p < s.size() ? s[p] : '\0'; }
    void advance() { if (s[p++] == '\n') ++line; }
    bool take(std::string_view token) {
        if (!s.substr(p).starts_with(token)) return false;
        for (std::size_t i = 0; i < token.size(); ++i) advance();
        return true;
    }
    void whitespace() {
        while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') advance();
    }
    void trivia() {
        for (;;) {
            whitespace();
            if (take("//")) {
                while (p < s.size() && peek() != '\n') advance();
            } else if (take("/*")) {
                while (p < s.size() && !s.substr(p).starts_with("*/")) advance();
                if (!take("*/")) fail("Unterminated comment");
            } else return;
        }
    }
    std::string headerField(std::string_view prefix) {
        if (!take(prefix)) fail("Invalid KV3 header field");
        auto start = p;
        while (p < s.size() && peek() != ':' && peek() != ' ' && peek() != '\n') advance();
        std::string name(s.substr(start, p - start));
        if (name.empty() || !take(":version{")) fail("Missing KV3 header version");
        start = p;
        while (p < s.size() && peek() != '}') advance();
        if (p == start || !take("}")) fail("Invalid KV3 header version");
        return name;
    }
    static bool first(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    std::string identifier() {
        auto start = p;
        if (!first(peek())) fail("Expected identifier");
        advance();
        while (first(peek()) || digit(peek()) || peek() == '.') advance();
        return std::string(s.substr(start, p - start));
    }
    std::string string() {
        if (take("\"\"\"")) {
            if (peek() == '\r') advance();
            if (!take("\n")) fail("Multiline opening quotes must be followed by a newline");
            auto start = p;
            while (p < s.size()) {
                const bool lineStart = p == start || s[p - 1] == '\n';
                if (lineStart) {
                    auto end = p;
                    while (peek() == ' ' || peek() == '\t') advance();
                    if (take("\"\"\"")) {
                        auto tail = p;
                        while (tail < s.size() && (s[tail] == ' ' || s[tail] == '\t')) ++tail;
                        if (tail < s.size() && s[tail] != '\r' && s[tail] != '\n')
                            fail("Multiline closing quotes must be on their own line");
                        if (end > start && s[end - 1] == '\n') --end;
                        if (end > start && s[end - 1] == '\r') --end;
                        return std::string(s.substr(start, end - start));
                    }
                }
                if (p < s.size()) advance();
            }
            fail("Unterminated multiline string");
        }
        if (!take("\"")) fail("Expected string");
        std::string out;
        while (p < s.size()) {
            char c = peek(); advance();
            if (c == '"') return out;
            if (c == '\n' || c == '\r') fail("Newline in quoted string");
            if (c == '\\') {
                if (p == s.size()) fail("Unterminated string escape");
                c = peek(); advance();
                switch (c) {
                case '"': case '\\': case '\'': case '?': break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                default: fail("Invalid string escape");
                }
            }
            out += c;
        }
        fail("Unterminated string");
    }
    Value value(std::size_t depth) {
        if (depth >= 256) fail("KV3 depth limit exceeded");
        trivia();
        Value v;
        v.source = {p, 0, line};
        if (first(peek())) {
            auto word = identifier();
            const auto end = p, endLine = line;
            trivia();
            if (take(":")) {
                const auto start = v.source;
                v = value(depth + 1);
                v.flag = std::move(word);
                // The flagged value includes its prefix for later source-preserving edits.
                v.source = {start.offset, p - start.offset, start.line};
                return v;
            }
            p = end;
            line = endLine;
            if (word == "null") v.data = nullptr;
            else if (word == "true" || word == "false") v.data = word == "true";
            else fail("Unknown value: " + word);
            v.source.length = end - v.source.offset;
            return v;
        } else if (peek() == '"') {
            v.data = string();
        } else if (take("{")) {
            Value::Object object;
            trivia();
            while (!take("}")) {
                auto start = p, keyLine = line;
                auto key = peek() == '"' ? string() : identifier();
                kv1::SourceSpan keySpan{start, p - start, keyLine};
                trivia();
                if (!take("=")) fail("Expected '='");
                object.push_back({std::move(key), keySpan, value(depth + 1)});
                trivia();
                take(",");
                trivia();
            }
            v.data = std::move(object);
        } else if (take("[")) {
            Value::Array array;
            trivia();
            while (!take("]")) {
                array.push_back(value(depth + 1));
                trivia(); take(","); trivia();
            }
            v.data = std::move(array);
        } else if (take("#[")) {
            Value::Binary bytes;
            trivia();
            while (!take("]")) {
                if (s.size() - p < 2) fail("Unterminated binary blob");
                unsigned byte = 0;
                auto parsed = std::from_chars(s.data() + p, s.data() + p + 2, byte, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != s.data() + p + 2) fail("Invalid binary byte");
                p += 2;
                bytes.push_back(static_cast<std::uint8_t>(byte));
                if (peek() != ']' && peek() != ' ' && peek() != '\t' && peek() != '\r' && peek() != '\n')
                    fail("Expected binary byte separator");
                trivia();
            }
            v.data = std::move(bytes);
        } else {
            auto start = p;
            if (peek() == '-' || peek() == '+') advance();
            if (!digit(peek())) fail("Expected value");
            while (digit(peek())) advance();
            bool real = false;
            if (take(".")) {
                real = true;
                if (!digit(peek())) fail("Expected fractional digits");
                while (digit(peek())) advance();
            }
            if (peek() == 'e' || peek() == 'E') {
                real = true; advance();
                if (peek() == '-' || peek() == '+') advance();
                if (!digit(peek())) fail("Expected exponent digits");
                while (digit(peek())) advance();
            }
            auto token = s.substr(start, p - start);
            if (token.front() == '+') token.remove_prefix(1);
            if (real) {
                double number = 0;
                auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
                if (parsed.ec != std::errc{} || !std::isfinite(number)) fail("Invalid floating point number");
                v.data = number;
            } else {
                std::int64_t number = 0;
                auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
                if (parsed.ec != std::errc{}) fail("Integer out of range");
                v.data = number;
            }
        }
        v.source.length = p - v.source.offset;
        return v;
    }
};
} // namespace

Result<Document> parse(std::string_view source) {
    try { return Parser(source).run(); }
    catch (const Error& error) { return std::unexpected(error); }
}

} // namespace apex::kv3
