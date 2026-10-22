#include "core/kv1_detail.hpp"

#include "core/paths.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <type_traits>
#include <utility>

namespace apex::kv1 {
namespace {

Error invalid(std::string message) {
    return {ErrorCode::InvalidArgument, std::move(message)};
}

template <typename T> Result<T> number(std::string_view text) {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+') {
            return std::unexpected(invalid("invalid number"));
        }
    }
    T result{};
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (status != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(invalid("invalid or out-of-range number"));
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(result)) {
            return std::unexpected(invalid("non-finite number"));
        }
    }
    return result;
}

std::vector<std::string_view> components(std::string_view text) {
    std::vector<std::string_view> result;
    text = trim(text);
    while (!text.empty()) {
        std::size_t end = 0;
        while (end < text.size() && !detail::space(text[end])) {
            ++end;
        }
        result.push_back(text.substr(0, end));
        text = trim(text.substr(end));
    }
    return result;
}

} // namespace

Node Node::value(std::string key, std::string decodedValue) {
    Node node;
    node.key_ = std::move(key);
    node.value_ = std::move(decodedValue);
    return node;
}

Node Node::block(std::string key) {
    Node node;
    node.kind_ = NodeKind::Block;
    node.key_ = std::move(key);
    return node;
}

std::size_t Node::memory() const {
    const auto allocated = [](const std::string& text) -> std::size_t {
        const auto data = reinterpret_cast<std::uintptr_t>(text.data());
        const auto object = reinterpret_cast<std::uintptr_t>(&text);
        return data >= object && data < object + sizeof(text) ? 0 : text.capacity() + 1;
    };
    std::size_t bytes = sizeof(*this) + allocated(key_) + allocated(condition_) +
                        (value_ ? allocated(*value_) : 0) + children_.capacity() * sizeof(Node);
    for (const auto& child : children_) bytes += child.memory() - sizeof(Node);
    return bytes;
}

std::string_view Node::slice(SourceSpan span) const {
    return source_ ? std::string_view(*source_).substr(span.offset, span.length)
                   : std::string_view{};
}

std::string_view Node::rawKey() const {
    return source_ ? slice(keySpan_) : std::string_view(key_);
}

std::optional<std::string_view> Node::rawValue() const {
    if (kind_ == NodeKind::Block) {
        return std::nullopt;
    }
    return source_ ? slice(valueSpan_)
                   : (value_ ? std::string_view(*value_) : std::string_view{});
}

Result<std::string> Node::decodedValue() const {
    if (kind_ == NodeKind::Block) {
        return std::unexpected(invalid("block has no scalar value"));
    }
    return value_ ? *value_
                  : detail::decode(slice(valueSpan_), valueQuoted_ ? mode_ : EscapeMode::Legacy);
}

Result<int> Node::asInt() const {
    auto text = decodedValue();
    if (!text) {
        return std::unexpected(text.error());
    }
    return number<int>(*text);
}

Result<double> Node::asDouble() const {
    auto text = decodedValue();
    if (!text) {
        return std::unexpected(text.error());
    }
    return number<double>(*text);
}

Result<Vec3> Node::asVec3() const {
    auto text = decodedValue();
    if (!text) {
        return std::unexpected(text.error());
    }
    auto view = trim(*text);
    if (view.size() >= 2 && view.front() == '[' && view.back() == ']') {
        view = view.substr(1, view.size() - 2);
    }
    const auto parts = components(view);
    if (parts.size() != 3) {
        return std::unexpected(invalid("expected three vector components"));
    }
    Vec3 result{};
    for (std::size_t i = 0; i < 3; ++i) {
        auto component = number<double>(parts[i]);
        if (!component) {
            return std::unexpected(component.error());
        }
        result[i] = *component;
    }
    return result;
}

Result<Color255> Node::asColor255() const {
    auto text = decodedValue();
    if (!text) {
        return std::unexpected(text.error());
    }
    const auto parts = components(*text);
    if (parts.size() != 3 && parts.size() != 4) {
        return std::unexpected(invalid("expected three or four color components"));
    }
    Color255 result{0, 0, 0, 255};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto component = number<int>(parts[i]);
        if (!component || *component < 0 || *component > 255) {
            return std::unexpected(invalid("color component outside 0..255"));
        }
        result[i] = static_cast<std::uint8_t>(*component);
    }
    return result;
}

Node* Node::find(std::string_view key) {
    for (auto& child : children_) {
        if (ciEquals(child.key(), key)) {
            return &child;
        }
    }
    return nullptr;
}

const Node* Node::find(std::string_view key) const {
    for (const auto& child : children_) {
        if (ciEquals(child.key(), key)) {
            return &child;
        }
    }
    return nullptr;
}

std::vector<Node*> Node::findAll(std::string_view key) {
    std::vector<Node*> result;
    for (auto& child : children_) {
        if (ciEquals(child.key(), key)) {
            result.push_back(&child);
        }
    }
    return result;
}

std::vector<const Node*> Node::findAll(std::string_view key) const {
    std::vector<const Node*> result;
    for (const auto& child : children_) {
        if (ciEquals(child.key(), key)) {
            result.push_back(&child);
        }
    }
    return result;
}

void Node::setKey(std::string key) {
    if (kind_ == NodeKind::Base || kind_ == NodeKind::Include) {
        kind_ = ciEquals(key, "#base") ? NodeKind::Base
                                      : (ciEquals(key, "#include") ? NodeKind::Include
                                                                    : NodeKind::Value);
    }
    key_ = std::move(key);
    dirty_ = true;
}

Result<void> Node::setValue(std::string decodedValue) {
    if (kind_ == NodeKind::Block) {
        return std::unexpected(invalid("cannot set a scalar value on a block"));
    }
    value_ = std::move(decodedValue);
    dirty_ = true;
    return {};
}

Result<void> Node::setCondition(std::string condition) {
    if (!detail::validCondition(condition)) {
        return std::unexpected(invalid("expected [$CONDITION] or [!$CONDITION]"));
    }
    condition_ = std::move(condition);
    dirty_ = true;
    return {};
}

void Node::detach() {
    dirty_ = true;
    leading_ = {};
    header_ = {};
    tail_ = {};
    span_ = {};
    for (auto& child : children_) {
        child.detach();
    }
}

Result<void> Node::append(Node node) {
    return insert(children_.size(), std::move(node));
}

Result<void> Node::insert(std::size_t index, Node node) {
    if (kind_ != NodeKind::Block || index > children_.size()) {
        return std::unexpected(invalid("invalid block insertion index"));
    }
    node.detach();
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(index), std::move(node));
    dirty_ = true;
    return {};
}

Result<void> Node::remove(std::size_t index) {
    if (kind_ != NodeKind::Block || index >= children_.size()) {
        return std::unexpected(invalid("invalid block removal index"));
    }
    children_.erase(children_.begin() + static_cast<std::ptrdiff_t>(index));
    dirty_ = true;
    return {};
}

bool Node::modified() const {
    return dirty_ || std::any_of(children_.begin(), children_.end(),
                                 [](const Node& child) { return child.modified(); });
}

Document::Document(EscapeMode mode)
    : root_(Node::block("")), mode_(mode), source_(std::make_shared<const std::string>()) {}

std::vector<const Node*> Document::directives() const {
    std::vector<const Node*> result;
    const auto collect = [&](const auto& self, const Node& parent) -> void {
        for (const auto& child : parent.children()) {
            if (child.kind() == NodeKind::Base || child.kind() == NodeKind::Include) {
                result.push_back(&child);
            } else if (child.kind() == NodeKind::Block) {
                self(self, child);
            }
        }
    };
    collect(collect, root_);
    return result;
}

Result<Document> Document::resolve(const Resolver& resolver) const {
    if (!resolver) {
        return std::unexpected(invalid("a directive resolver is required"));
    }
    std::vector<std::string> active;
    std::size_t loads = 0;
    const auto merge = [&](const auto& self, Node& target, const Node& base) -> void {
        // Preserve every duplicate base key that was missing before this merge.
        const auto localCount = static_cast<std::ptrdiff_t>(target.children_.size());
        for (const auto& child : base.children_) {
            const auto end = target.children_.begin() + localCount;
            const auto found = std::find_if(target.children_.begin(), end,
                                            [&](const Node& candidate) {
                                                return ciEquals(candidate.key(), child.key());
                                            });
            if (found == end) {
                target.children_.push_back(child);
            } else if (found->kind() == NodeKind::Block && child.kind() == NodeKind::Block) {
                self(self, *found, child);
            }
        }
    };
    const auto expand = [&](const auto& self, const Node& parent,
                            std::size_t depth) -> Result<Node> {
        if (depth > 256) {
            return std::unexpected(Error{ErrorCode::ParseError,
                                          "resolved block nesting exceeds 256"});
        }
        Node result = parent;
        result.children_.clear();
        std::vector<Node> includes;
        std::vector<Node> bases;
        for (const auto& child : parent.children_) {
            if (child.kind_ == NodeKind::Base || child.kind_ == NodeKind::Include) {
                const auto path = child.decodedValue();
                if (!path) {
                    return std::unexpected(path.error());
                }
                const auto lookup = lookupKey(*path);
                if (active.size() >= 64 || ++loads > 4096 ||
                    std::find(active.begin(), active.end(), lookup) != active.end()) {
                    return std::unexpected(Error{ErrorCode::ParseError,
                                                  "directive cycle or expansion limit"}
                                               .withContext(*path));
                }
                auto source = resolver(*path);
                if (!source) {
                    return std::unexpected(source.error().withContext(*path));
                }
                auto parsed = parse(std::move(*source), mode_);
                if (!parsed) {
                    return std::unexpected(parsed.error().withContext(*path));
                }
                active.push_back(lookup);
                auto resolved = self(self, parsed->root_, depth);
                active.pop_back();
                if (!resolved) {
                    return std::unexpected(resolved.error().withContext(*path));
                }
                (child.kind_ == NodeKind::Base ? bases : includes).push_back(std::move(*resolved));
            } else if (child.kind_ == NodeKind::Block) {
                auto block = self(self, child, depth + 1);
                if (!block) {
                    return std::unexpected(block.error());
                }
                result.children_.push_back(std::move(*block));
            } else {
                result.children_.push_back(child);
            }
        }
        for (const auto& included : includes) {
            result.children_.insert(result.children_.end(), included.children_.begin(),
                                    included.children_.end());
        }
        for (const auto& base : bases) {
            merge(merge, result, base);
        }
        return result;
    };
    auto expanded = expand(expand, root_, 0);
    if (!expanded) {
        return std::unexpected(expanded.error());
    }
    Document result(mode_);
    result.newline_ = newline_;
    result.root_ = std::move(*expanded);
    result.root_.detach();
    return result;
}

Result<FrozenNode> FrozenNode::freeze(const Node& node, EscapeMode mode, std::string_view newline) {
    auto wrapper = parse(std::string(newline), mode);
    if (!wrapper) return std::unexpected(wrapper.error());
    wrapper->root().append(node).value();
    auto text = write(*wrapper);
    if (!text) return std::unexpected(text.error());
    auto parsed = parse(*text, mode);
    if (!parsed) return std::unexpected(parsed.error());
    if (parsed->root().children().size() != 1)
        return std::unexpected(invalid("snapshot is not one KV1 node"));
    FrozenNode result;
    result.mode_ = mode;
    result.text_ = std::move(*text);
    const auto number = [&](std::size_t value) {
        do {
            auto byte = static_cast<unsigned char>(value & 127);
            value >>= 7;
            if (value) byte |= 128;
            result.provenance_ += static_cast<char>(byte);
        } while (value);
    };
    const auto encode = [&](const auto& self, const Node& original, const Node& copy) -> bool {
        if (original.kind_ != copy.kind_ || original.key_ != copy.key_ ||
            original.condition_ != copy.condition_ || original.children_.size() != copy.children_.size())
            return false;
        if (original.kind_ != NodeKind::Block && original.decodedValue().value() != copy.decodedValue().value())
            return false;
        // Preserve mode per node: newly inserted nodes may use a different mode from the file.
        number((original.dirty_ ? 1U : 0U) | (original.valueQuoted_ ? 2U : 0U) |
               (original.value_ ? 4U : 0U) | (original.mode_ == EscapeMode::Legacy ? 8U : 0U));
        std::size_t source = 0;
        if (original.source_) {
            const auto found = std::ranges::find(result.sources_, original.source_);
            source = static_cast<std::size_t>(found - result.sources_.begin()) + 1;
            if (found == result.sources_.end()) result.sources_.push_back(original.source_);
        }
        number(source);
        for (const auto span : {original.span_, original.keySpan_, original.valueSpan_,
                                original.leading_, original.header_, original.tail_}) {
            number(span.offset);
            number(span.length);
            number(span.line);
        }
        for (std::size_t i = 0; i < original.children_.size(); ++i)
            if (!self(self, original.children_[i], copy.children_[i])) return false;
        return true;
    };
    if (!encode(encode, node, parsed->root().children()[0]))
        return std::unexpected(invalid("KV1 snapshot changes node structure or values"));
    result.text_.shrink_to_fit();
    result.provenance_.shrink_to_fit();
    result.sources_.shrink_to_fit();
    return result;
}

Result<Node> FrozenNode::thaw() const {
    auto parsed = parse(text_, mode_);
    if (!parsed) return std::unexpected(parsed.error());
    auto result = std::move(parsed->root().children()[0]);
    std::size_t offset = 0;
    const auto number = [&] {
        std::size_t value = 0;
        unsigned shift = 0;
        unsigned char byte;
        do {
            byte = static_cast<unsigned char>(provenance_.at(offset++));
            value |= std::size_t(byte & 127) << shift;
            shift += 7;
        } while (byte & 128);
        return value;
    };
    const auto decode = [&](const auto& self, Node& node) -> void {
        const auto flags = number();
        // Capture the parsed value before restoring the original source spans.
        if (flags & 4) node.value_ = node.decodedValue().value();
        else node.value_.reset();
        node.dirty_ = flags & 1;
        node.valueQuoted_ = flags & 2;
        node.mode_ = flags & 8 ? EscapeMode::Legacy : EscapeMode::Strata;
        const auto source = number();
        node.source_ = source ? sources_.at(source - 1) : nullptr;
        for (auto* span : {&node.span_, &node.keySpan_, &node.valueSpan_,
                           &node.leading_, &node.header_, &node.tail_}) {
            span->offset = number();
            span->length = number();
            span->line = number();
        }
        for (auto& child : node.children_) self(self, child);
    };
    decode(decode, result);
    return result;
}

std::size_t FrozenNode::memory() const {
    return sizeof(*this) + text_.capacity() + provenance_.capacity() +
           sources_.capacity() * sizeof(sources_[0]);
}

} // namespace apex::kv1
