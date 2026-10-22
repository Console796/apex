#pragma once

#include "core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::kv1 {

enum class EscapeMode { Strata, Legacy };
enum class TokenKind { Text, OpenBrace, CloseBrace, Condition, Comment, Whitespace };
enum class NodeKind { Value, Block, Base, Include };

struct SourceSpan {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t line = 1;
};

struct Token {
    TokenKind kind;
    SourceSpan span;
    bool quoted = false;
};

// All tokens, including trivia, partition the input. Spans count bytes, lines start at one.
Result<std::vector<Token>> tokenize(std::string_view source,
                                   EscapeMode mode = EscapeMode::Strata);

using Resolver = std::function<Result<std::string>(std::string_view path)>;
using Vec3 = std::array<double, 3>;
using Color255 = std::array<std::uint8_t, 4>;
class FrozenNode;

class Node {
public:
    static Node value(std::string key, std::string decodedValue);
    static Node block(std::string key);

    NodeKind kind() const { return kind_; }
    std::string_view key() const { return key_; }
    SourceSpan sourceSpan() const { return span_; }
    // Raw token bodies exclude surrounding quotes; escapes are retained, even after mutation.
    std::string_view rawKey() const;
    std::optional<std::string_view> rawValue() const;
    std::string_view condition() const { return condition_; }
    Result<std::string> decodedValue() const;
    Result<int> asInt() const;
    Result<double> asDouble() const;
    Result<Vec3> asVec3() const;
    Result<Color255> asColor255() const;

    std::span<Node> children() { return children_; }
    std::span<const Node> children() const { return children_; }
    Node* find(std::string_view key);
    const Node* find(std::string_view key) const;
    std::vector<Node*> findAll(std::string_view key);
    std::vector<const Node*> findAll(std::string_view key) const;

    void setKey(std::string key);
    Result<void> setValue(std::string decodedValue);
    // Empty removes the suffix; otherwise use the full syntax, e.g. "[!$WIN32]".
    Result<void> setCondition(std::string condition);
    // Structural edits invalidate child pointers/references, like std::vector edits.
    Result<void> append(Node node);
    Result<void> insert(std::size_t index, Node node);
    Result<void> remove(std::size_t index);
    bool modified() const;
    // Node storage and owned allocations; shared input buffers are owned by the document.
    std::size_t memory() const;

private:
    friend class Parser;
    friend class Writer;
    friend class Document;
    friend class FrozenNode;
    void detach();
    std::string_view slice(SourceSpan span) const;

    NodeKind kind_ = NodeKind::Value;
    std::string key_;
    std::string condition_;
    std::optional<std::string> value_;
    std::vector<Node> children_;
    std::shared_ptr<const std::string> source_;
    EscapeMode mode_ = EscapeMode::Strata;
    SourceSpan span_, keySpan_, valueSpan_, leading_, header_, tail_;
    bool valueQuoted_ = false;
    bool dirty_ = true;
};

class Document {
public:
    // A new document uses CRLF. Parsed documents use the first LF's style (CRLF or LF).
    explicit Document(EscapeMode mode = EscapeMode::Strata);
    Node& root() { return root_; }
    const Node& root() const { return root_; }
    EscapeMode escapeMode() const { return mode_; }
    std::string_view source() const { return *source_; }
    std::string_view lineEnding() const { return newline_; }
    std::span<const Token> tokens() const { return tokens_; }
    std::vector<const Node*> directives() const;

    // Returns a separate expanded document, leaving this document lossless. Includes append
    // nodes; bases recursively fill missing keys (first case-insensitive match). Conditions
    // remain unevaluated. The resolver receives literal decoded paths, with no filesystem I/O.
    // Cycles and excessive nesting are errors; callers own path lookup and conditional policy.
    Result<Document> resolve(const Resolver& resolver) const;

private:
    friend class Parser;
    friend class Writer;
    Node root_;
    EscapeMode mode_;
    std::string newline_ = "\r\n";
    std::shared_ptr<const std::string> source_;
    std::vector<Token> tokens_;
};

// A maximum of 256 nested blocks bounds parser/writer stack usage on hostile inputs.
Result<Document> parse(std::string source, EscapeMode mode = EscapeMode::Strata);
// Legacy cannot encode a newly supplied literal quote; write returns Unsupported in that case.
Result<std::string> write(const Document& document);

// In-process, lossless snapshot: KV1 text plus compact writer provenance. Source buffers
// are shared, never copied. The sidecar preserves formatting on subsequent edits too.
// Refuses states that cannot reparse with the same structure and values.
class FrozenNode {
public:
    static Result<FrozenNode> freeze(const Node& node, EscapeMode mode, std::string_view newline);
    Result<Node> thaw() const;
    std::size_t memory() const;
private:
    FrozenNode() = default;
    std::string text_, provenance_;
    std::vector<std::shared_ptr<const std::string>> sources_;
    EscapeMode mode_ = EscapeMode::Strata;
};

} // namespace apex::kv1
