#pragma once

#include "editor/connection_validation.hpp"

namespace apex::editor {

std::vector<std::string> outputsFor(const assets::EntityClassTable& classes, std::string_view classname);
std::vector<std::string> targetsFor(const NameIndex& names, std::string_view prefix = {});
std::vector<std::string> inputsFor(const ConnectionValidator& context, std::string_view targetText);
std::vector<std::string> parameterChoices(const assets::EntityClassTable& classes,
    std::string_view inputType, const NameIndex& names);

} // namespace apex::editor
