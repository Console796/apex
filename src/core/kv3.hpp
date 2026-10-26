#pragma once

#include "core/kv1.hpp"

#include <variant>

namespace apex::kv3 {

struct Member;
struct Value {
    enum class Kind { Null, Bool, Int, Double, String, Array, Object, Binary };
    using Array = std::vector<Value>;
    using Object = std::vector<Member>;
    using Binary = std::vector<std::uint8_t>;
    // No default initializer: it would instantiate the variant while Member is still incomplete (Clang).
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object, Binary> data;
    std::string flag;
    kv1::SourceSpan source;

    Kind kind() const { return static_cast<Kind>(data.index()); }
    template <typename T> const T* get() const { return std::get_if<T>(&data); }
    const Value* find(std::string_view key) const;
    kv1::SourceSpan span() const { return source; }
};
struct Member {
    std::string key;
    kv1::SourceSpan keySpan;
    Value value;
};
struct Document {
    std::string encoding;
    std::string format;
    Value root;
};

// Text only; spans count source bytes and lines start at one. Nesting is bounded at 256.
Result<Document> parse(std::string_view source);

} // namespace apex::kv3
