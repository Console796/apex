#pragma once
#include "apex_plugin.h"
#include <string_view>
namespace apex::plugin {
inline apex_str string(std::string_view text) { return {text.data(), text.size()}; }
}
