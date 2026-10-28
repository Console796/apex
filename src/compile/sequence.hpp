#pragma once

#include <optional>
#include <string>
#include <vector>

namespace apex::compile {

enum class SpecialCommand { ChangeDirectory = 256, Copy, Delete, Rename, CopyIfExists = 261 };

struct Command {
    std::string executable;
    std::string arguments;
    bool enabled = true;
    bool wait = true;
    std::optional<SpecialCommand> special = std::nullopt;
    bool operator==(const Command&) const = default;
};

struct Sequence {
    std::string name;
    std::vector<Command> commands;
    bool operator==(const Sequence&) const = default;
};

struct CompileProfile {
    std::string name;
    std::vector<Command> commands;
    bool runGameAfter = true;
    bool copyBspToMaps = true;
    bool cordonOnly = false;
    std::string cordonMaterial = "tools/toolsskybox"; // Material of the six walls a cordon compile adds (D7).
    bool operator==(const CompileProfile&) const = default;
};

std::vector<CompileProfile> builtinProfiles();
bool isToolVariable(const std::string& executable);

} // namespace apex::compile
