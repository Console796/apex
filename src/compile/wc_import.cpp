#include "compile/wc_import.hpp"

#include <algorithm>
#include <toolpp/CmdSeq.h>

namespace apex::compile {

Result<std::vector<CompileProfile>> importWc(const std::filesystem::path& file) {
    toolpp::CmdSeq input(file);
    if (!input || input.getSequences().empty())
        return std::unexpected(Error{ErrorCode::ParseError, "Cannot import command sequences: " + file.string()});
    std::vector<CompileProfile> profiles;
    for (const auto& sequence : input.getSequences()) {
        CompileProfile profile;
        profile.name = sequence.name;
        for (const auto& source : sequence.commands) {
            Command command{source.executable, source.arguments, source.enabled, !source.waitForKeypress, {}};
            const auto special = static_cast<int>(source.special);
            if (special != 0) {
                switch (special) {
                case 256: case 257: case 258: case 259: case 261:
                    command.special = static_cast<SpecialCommand>(special);
                    break;
                default: return std::unexpected(Error{ErrorCode::Unsupported, "Unknown .wc special command " + std::to_string(special)});
                }
            }
            profile.commands.push_back(std::move(command));
        }
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

std::vector<std::string> listNonToolExecutables(const CompileProfile& profile) {
    std::vector<std::string> result;
    for (const auto& command : profile.commands) {
        if (!command.enabled) continue;
        const auto name = command.special ? toolpp::CmdSeq::Command::getSpecialDisplayNameFor(
            static_cast<toolpp::CmdSeq::Command::Special>(*command.special)) : command.executable;
        if ((command.special || !isToolVariable(name)) && std::ranges::find(result, name) == result.end())
            result.push_back(name);
    }
    return result;
}

std::string exportWc(const CompileProfile& profile) {
    toolpp::CmdSeq output(toolpp::CmdSeq::Type::KEYVALUES_STRATA);
    toolpp::CmdSeq::Sequence sequence;
    sequence.name = profile.name;
    for (const auto& command : profile.commands) {
        toolpp::CmdSeq::Command target{};
        target.enabled = command.enabled;
        target.special = command.special ? static_cast<toolpp::CmdSeq::Command::Special>(*command.special) :
                                          toolpp::CmdSeq::Command::Special::NONE;
        target.executable = command.executable;
        target.arguments = command.arguments;
        target.waitForKeypress = !command.wait;
        sequence.commands.push_back(std::move(target));
    }
    output.getSequences().push_back(std::move(sequence));
    const auto bytes = output.bake();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

} // namespace apex::compile
