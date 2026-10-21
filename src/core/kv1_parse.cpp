#include "core/kv1_detail.hpp"

#include "core/strings.hpp"

#include <utility>

namespace apex::kv1 {

Result<std::vector<Token>> tokenize(std::string_view source, EscapeMode mode) {
    std::vector<Token> tokens;
    std::size_t offset = 0;
    std::size_t line = 1;
    while (offset < source.size()) {
        const auto start = offset;
        const auto startLine = line;
        TokenKind kind = TokenKind::Text;
        bool quoted = false;
        const auto advance = [&] {
            if (source[offset] == '\n') {
                ++line;
            }
            ++offset;
        };
        if (detail::space(source[offset])) {
            kind = TokenKind::Whitespace;
            while (offset < source.size() && detail::space(source[offset])) {
                advance();
            }
        } else if (source.substr(offset, 2) == "//") {
            kind = TokenKind::Comment;
            while (offset < source.size() && source[offset] != '\n' && source[offset] != '\r') {
                advance();
            }
        } else if (source[offset] == '{' || source[offset] == '}') {
            kind = source[offset] == '{' ? TokenKind::OpenBrace : TokenKind::CloseBrace;
            advance();
        } else if (detail::conditionStart(source, offset)) {
            kind = TokenKind::Condition;
            while (offset < source.size() && source[offset] != ']' && source[offset] != '\n') {
                advance();
            }
            if (offset < source.size() && source[offset] == ']') {
                advance();
            }
            if (!detail::validCondition(source.substr(start, offset - start))) {
                return std::unexpected(detail::errorAt(source, {start, offset - start, startLine},
                                                       "malformed condition"));
            }
        } else if (source[offset] == '"') {
            quoted = true;
            advance();
            bool closed = false;
            while (offset < source.size()) {
                if (source[offset] == '"') {
                    advance();
                    closed = true;
                    break;
                }
                if (mode == EscapeMode::Strata && source[offset] == '\\') {
                    advance();
                    if (offset < source.size()) {
                        advance();
                    }
                } else {
                    advance();
                }
            }
            if (!closed) {
                return std::unexpected(detail::errorAt(source, {start, offset - start, startLine},
                                                       "unterminated string"));
            }
        } else {
            do {
                advance();
            } while (offset < source.size() && !detail::space(source[offset]) &&
                     source[offset] != '{' && source[offset] != '}' && source[offset] != '"' &&
                     source.substr(offset, 2) != "//" && !detail::conditionStart(source, offset));
        }
        tokens.push_back({kind, {start, offset - start, startLine}, quoted});
    }
    return tokens;
}

class Parser {
public:
    Parser(std::string source, EscapeMode mode) : document_(mode) {
        document_.source_ = std::make_shared<const std::string>(std::move(source));
        const auto lf = document_.source_->find('\n');
        if (lf != std::string::npos) {
            document_.newline_ = lf > 0 && (*document_.source_)[lf - 1] == '\r' ? "\r\n" : "\n";
        }
    }

    Result<Document> run() {
        auto tokens = tokenize(*document_.source_, document_.mode_);
        if (!tokens) {
            return std::unexpected(tokens.error());
        }
        document_.tokens_ = std::move(*tokens);
        auto& root = document_.root_;
        initialize(root);
        auto status = children(root, 0, 0, true);
        if (!status) {
            return std::unexpected(status.error());
        }
        root.span_ = {0, document_.source_->size(), 1};
        return std::move(document_);
    }

private:
    void initialize(Node& node) const {
        node.source_ = document_.source_;
        node.mode_ = document_.mode_;
        node.dirty_ = false;
    }

    const Token* peek() {
        while (index_ < document_.tokens_.size()) {
            const auto& token = document_.tokens_[index_];
            if (token.kind != TokenKind::Whitespace && token.kind != TokenKind::Comment) {
                return &token;
            }
            ++index_;
        }
        return nullptr;
    }

    Error error(std::string message) {
        const auto* token = peek();
        if (token) {
            return detail::errorAt(*document_.source_, token->span, std::move(message));
        }
        const auto& tokens = document_.tokens_;
        std::size_t line = 1;
        if (!tokens.empty()) {
            const auto& last = tokens.back();
            line = last.span.line;
            for (const char c : std::string_view(*document_.source_).substr(last.span.offset)) {
                line += c == '\n' ? 1U : 0U;
            }
        }
        return detail::errorAt(*document_.source_, {document_.source_->size(), 0, line},
                               std::move(message));
    }

    static SourceSpan body(const Token& token) {
        auto span = token.span;
        if (token.quoted) {
            ++span.offset;
            span.length -= 2;
        }
        return span;
    }

    bool condition(Node& node, std::size_t& end) {
        const auto* token = peek();
        if (token && token->kind == TokenKind::Condition) {
            node.condition_ = node.slice(token->span);
            end = token->span.offset + token->span.length;
            ++index_;
            return true;
        }
        return false;
    }

    Result<void> children(Node& parent, std::size_t begin, std::size_t depth, bool root) {
        auto previous = begin;
        while (const auto* token = peek()) {
            if (token->kind == TokenKind::CloseBrace && !root) {
                parent.tail_ = {previous, token->span.offset - previous, token->span.line};
                return {};
            }
            if (token->kind != TokenKind::Text) {
                return std::unexpected(error("expected a key"));
            }
            Node node;
            initialize(node);
            node.keySpan_ = body(*token);
            node.key_ = detail::decode(node.slice(node.keySpan_),
                                       token->quoted ? node.mode_ : EscapeMode::Legacy);
            node.span_ = token->span;
            node.leading_ = {previous, token->span.offset - previous, token->span.line};
            const bool directive = !token->quoted &&
                                   (ciEquals(node.key_, "#base") ||
                                    ciEquals(node.key_, "#include"));
            ++index_;
            auto end = node.span_.offset + node.span_.length;
            const bool beforeBlock = condition(node, end);
            token = peek();
            if (!token) {
                return std::unexpected(error("expected a value or block"));
            }
            if (token->kind == TokenKind::OpenBrace && !directive) {
                if (depth >= 256) {
                    return std::unexpected(error("block nesting exceeds 256"));
                }
                node.kind_ = NodeKind::Block;
                const auto openEnd = token->span.offset + token->span.length;
                node.header_ = {node.span_.offset, openEnd - node.span_.offset, node.span_.line};
                ++index_;
                auto status = children(node, openEnd, depth + 1, false);
                if (!status) {
                    return status;
                }
                token = peek();
                if (!token) {
                    return std::unexpected(error("unterminated block"));
                }
                end = token->span.offset + token->span.length;
                ++index_;
                if (!beforeBlock) {
                    condition(node, end);
                }
            } else if (token->kind == TokenKind::Text && !beforeBlock) {
                node.kind_ = directive ? (ciEquals(node.key_, "#base") ? NodeKind::Base
                                                                     : NodeKind::Include)
                                       : NodeKind::Value;
                node.valueSpan_ = body(*token);
                node.valueQuoted_ = token->quoted;
                end = token->span.offset + token->span.length;
                ++index_;
                condition(node, end);
            } else {
                return std::unexpected(error("expected a value or block"));
            }
            node.span_.length = end - node.span_.offset;
            previous = end;
            parent.children_.push_back(std::move(node));
        }
        if (!root) {
            return std::unexpected(error("unterminated block"));
        }
        parent.tail_ = {previous, document_.source_->size() - previous, 1};
        return {};
    }

    Document document_;
    std::size_t index_ = 0;
};

Result<Document> parse(std::string source, EscapeMode mode) {
    return Parser(std::move(source), mode).run();
}

} // namespace apex::kv1
