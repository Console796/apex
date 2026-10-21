#include "core/kv1_detail.hpp"

namespace apex::kv1 {

class Writer {
public:
    explicit Writer(const Document& document) : document_(document) {}

    Result<std::string> run() {
        if (!document_.root_.modified()) {
            return std::string(document_.source());
        }
        auto status = children(document_.root_, 0, true);
        if (!status) {
            return std::unexpected(status.error());
        }
        return std::move(output_);
    }

private:
    void newline() { output_ += document_.newline_; }
    void indent(std::size_t depth) { output_.append(depth, '\t'); }

    Result<void> quote(std::string_view text) {
        output_ += '"';
        for (const char c : text) {
            if (document_.mode_ == EscapeMode::Legacy) {
                if (c == '"') {
                    return std::unexpected(Error{ErrorCode::Unsupported,
                                                  "Legacy mode cannot encode a literal quote"});
                }
                output_ += c;
            } else {
                switch (c) {
                case '\n': output_ += "\\n"; break;
                case '\t': output_ += "\\t"; break;
                case '"': output_ += "\\\""; break;
                case '\\': output_ += "\\\\"; break;
                default: output_ += c; break;
                }
            }
        }
        output_ += '"';
        return {};
    }

    void leading(const Node& node, std::size_t depth, bool first, bool root,
                 bool previousWasNew) {
        const auto text = node.slice(node.leading_);
        if (!node.modified()) {
            if (previousWasNew && text.empty()) {
                newline();
                indent(depth);
            }
            output_ += text;
            return;
        }
        const auto lf = text.rfind('\n');
        const auto suffix = lf == std::string_view::npos ? 0 : lf + 1;
        bool whitespace = true;
        for (const char c : text.substr(suffix)) {
            whitespace = whitespace && detail::space(c);
        }
        if (whitespace) {
            output_ += text.substr(0, suffix);
        } else {
            output_ += text;
        }
        if ((!root || !first || !whitespace) && !output_.empty() && output_.back() != '\n') {
            newline();
        }
        indent(depth);
    }

    Result<void> children(const Node& parent, std::size_t depth, bool root) {
        bool first = true;
        bool previousWasNew = false;
        for (const auto& child : parent.children_) {
            leading(child, depth, first, root, previousWasNew);
            auto status = node(child, depth);
            if (!status) {
                return status;
            }
            first = false;
            previousWasNew = child.span_.length == 0;
        }
        const auto tail = parent.slice(parent.tail_);
        output_ += tail;
        if (root) {
            if (previousWasNew && (output_.empty() || output_.back() != '\n')) {
                newline();
            }
        } else {
            // A changed inline block needs a separate closing-brace line too. Existing tail
            // comments and newlines survive; only the indentation before the brace changes.
            const auto lf = tail.rfind('\n');
            const auto suffix = lf == std::string_view::npos ? 0 : lf + 1;
            bool whitespace = true;
            for (const char c : tail.substr(suffix)) {
                whitespace = whitespace && detail::space(c);
            }
            if (whitespace) {
                output_.resize(output_.size() - (tail.size() - suffix));
            }
            if (!output_.empty() && output_.back() != '\n') {
                newline();
            }
            indent(depth - 1);
        }
        return {};
    }

    Result<void> node(const Node& current, std::size_t depth) {
        if (depth > 256) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "block nesting exceeds 256"});
        }
        if (!current.modified()) {
            output_ += current.slice(current.span_);
            return {};
        }
        if (!current.dirty_ && current.kind_ == NodeKind::Block) {
            output_ += current.slice(current.header_);
        } else {
            Result<void> status;
            if (current.kind_ == NodeKind::Include || current.kind_ == NodeKind::Base) {
                output_ += current.key_;
            } else {
                status = quote(current.key_);
            }
            if (!status) {
                return status;
            }
            if (current.kind_ != NodeKind::Block) {
                const auto value = current.decodedValue();
                if (!value) {
                    return std::unexpected(value.error());
                }
                output_ += ' ';
                status = quote(*value);
                if (!status) {
                    return status;
                }
            }
            if (!current.condition_.empty()) {
                output_ += ' ';
                output_ += current.condition_;
            }
            if (current.kind_ == NodeKind::Block) {
                newline();
                indent(depth);
                output_ += '{';
            }
        }
        if (current.kind_ == NodeKind::Block) {
            auto status = children(current, depth + 1, false);
            if (!status) {
                return status;
            }
            if (!current.dirty_ && current.span_.length != 0) {
                const auto close = current.tail_.offset + current.tail_.length;
                const auto length = current.span_.offset + current.span_.length - close;
                output_ += current.slice({close, length, current.span_.line});
            } else {
                output_ += '}';
            }
        }
        return {};
    }

    const Document& document_;
    std::string output_;
};

Result<std::string> write(const Document& document) {
    return Writer(document).run();
}

} // namespace apex::kv1
