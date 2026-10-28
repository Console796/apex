#pragma once

#include "compile/sequence.hpp"
#include "core/result.hpp"

#include <filesystem>

namespace apex::compile {

Result<std::vector<CompileProfile>> importWc(const std::filesystem::path& file);
std::vector<std::string> listNonToolExecutables(const CompileProfile& profile);
std::string exportWc(const CompileProfile& profile);

} // namespace apex::compile
