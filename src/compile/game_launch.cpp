#include "compile/game_launch.hpp"

#include <algorithm>

namespace apex::compile {

std::string defaultGameArguments() { return "-dev -console -allowdebug -game $gamedir +map $file"; }

std::vector<std::string> withConsoleLog(std::vector<std::string> arguments) {
    if (std::ranges::find(arguments, "-condebug") == arguments.end()) arguments.emplace_back("-condebug");
    return arguments;
}

std::filesystem::path gameLogPath(const std::filesystem::path& gameDir) { return gameDir / "console.log"; }

Result<GameLaunch> buildGameLaunch(const GameLaunchRequest& request) {
    const auto invalid = [](std::string message) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
    };
    if (request.gameExe.empty()) return invalid("No game executable is configured");
    if (request.gameDir.empty()) return invalid("No game directory is configured");
    if (request.mapName.empty() || request.mapName.find_first_of("/\\") != std::string::npos)
        return invalid("The map name must be a bare file name: " + request.mapName);
    Variables variables{{"gamedir", request.gameDir.string()}, {"file", request.mapName},
                        {"mapname", request.mapName}, {"game_exe", request.gameExe.string()}};
    auto arguments = expandArguments(request.arguments.empty() ? defaultGameArguments() : request.arguments, variables);
    if (!arguments) return std::unexpected(arguments.error());
    if (!request.campaign.empty()) {
        arguments->emplace_back("-campaign");
        arguments->push_back(request.campaign);
    }
    GameLaunch launch;
    launch.program = request.gameExe.string();
    launch.arguments = withConsoleLog(std::move(*arguments));
    launch.logFile = gameLogPath(request.gameDir);
    launch.bspFile = request.gameDir / "maps" / (request.mapName + ".bsp");
    return launch;
}

} // namespace apex::compile
