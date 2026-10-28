#pragma once

// Readers for the files vbsp and vvis leave in the work directory (0.5 D6): the leak path
// (`.lin`) and the portal file (`.prt`, "PRT1"). Both tolerate empty and truncated input and keep
// whatever was complete.

#include "core/math.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace apex::compile {

struct PortalPolygon {
    int clusterA = 0, clusterB = 0;
    std::vector<DVec3> points;
};

struct PortalFile {
    int clusters = 0;
    std::vector<PortalPolygon> portals;
};

// One `x y z` point per line, from the first entity to the outside. Lines that do not hold three numbers end the path.
std::vector<DVec3> parseLeakFile(std::string_view text);
// An absent or wrong header gives an empty file. A portal with a missing point or cluster is dropped, with everything after it.
PortalFile parsePortalFile(std::string_view text);

// The files a compile of `name` left in `directory`; nullopt when the file does not exist.
std::optional<std::vector<DVec3>> readLeakFile(const std::filesystem::path& directory, const std::string& name);
std::optional<PortalFile> readPortalFile(const std::filesystem::path& directory, const std::string& name);

} // namespace apex::compile
