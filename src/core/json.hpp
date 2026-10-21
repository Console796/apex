#pragma once

#include "core/result.hpp"

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace apex::json {

struct Value {
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data = nullptr;
    template <typename T> const T* get() const { return std::get_if<T>(&data); }
};

Result<Value> parse(std::string_view source);
std::string write(const Value& value);

} // namespace apex::json
