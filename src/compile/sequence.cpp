#include "compile/sequence.hpp"

#include <algorithm>
#include <array>

namespace apex::compile {

bool isToolVariable(const std::string& executable) {
    constexpr std::array tools{"$bsp_exe", "$vis_exe", "$light_exe", "$game_exe", "$postcompiler_exe"};
    return std::ranges::find(tools, executable) != tools.end();
}

std::vector<CompileProfile> builtinProfiles() {
    std::vector<CompileProfile> profiles;
    for (const auto* name : {"Fast", "Normal", "Final"}) {
        CompileProfile profile;
        profile.name = name;
        const bool fast = profile.name == "Fast";
        const std::string map = "-game $gamedir $path\\$file";
        profile.commands = {
            {"$bsp_exe", map},
            {"$postcompiler_exe", "-game $gamedir --propcombine $path\\$file"},
            {"$vis_exe", (fast ? "-fast " : "") + map},
            {"$light_exe", std::string(fast ? "-fast " : profile.name == "Final" ? "-bounce 100 -hdr " : "") +
                           "-StaticPropLighting -StaticPropPolys " + map},
            {"", "$path\\$file.bsp $bspdir\\$file.bsp", true, true, SpecialCommand::Copy},
            {"$game_exe", "-dev -console -allowdebug -game $gamedir +map $file", true, false},
        };
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

} // namespace apex::compile
