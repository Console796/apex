#pragma once

#include "core/result.hpp"

#include <map>
#include <string>
#include <string_view>

namespace apex::compile {

using Variables = std::map<std::string, std::string, std::less<>>;
// Values are inserted literally: whitespace and quotes inside a value never split an argument.
Result<std::string> expandVariables(std::string_view text, const Variables& variables);
Result<std::vector<std::string>> expandArguments(std::string_view text, const Variables& variables);

} // namespace apex::compile
