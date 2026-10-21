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

} // namespace apex::kv1
