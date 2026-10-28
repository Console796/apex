#pragma once

#include "compile/variables.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace apex::compile {

// 0.5 D9: the game step. The arguments are a template over the compile variables, so a profile can
// change them; the launcher script (p2ce.sh, p2ce.exe) supplies the rest.
struct GameLaunchRequest {
    std::filesystem::path gameExe;
    std::filesystem::path gameDir;
    std::string mapName;   // the BSP's name without directory or extension
    std::string arguments; // empty: defaultGameArguments()
    std::string campaign;  // "<bucket>/<id>" adds -campaign (D10); empty for none
};

struct GameLaunch {
    std::string program;
    std::vector<std::string> arguments;
    std::filesystem::path logFile; // where -condebug writes: <gamedir>/console.log
    std::filesystem::path bspFile; // <gamedir>/maps/<name>.bsp, the map the game is asked to load
};

// +map is the console command the engine reads from the command line; -game is added by the
// launcher script as well, and the later value wins.
std::string defaultGameArguments();
// Appends -condebug when absent so the game's console log exists for the Game log view.
std::vector<std::string> withConsoleLog(std::vector<std::string> arguments);
std::filesystem::path gameLogPath(const std::filesystem::path& gameDir);
Result<GameLaunch> buildGameLaunch(const GameLaunchRequest& request);

} // namespace apex::compile
