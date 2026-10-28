#pragma once

// What a finished compile left to look at (0.5 D6): the leak path and the portals, belonging to the
// document revision that was compiled. The overlay is a record of what the compiler saw and never moves
// with edits; it is greyed once the document has moved on.

#include "compile/map_files.hpp"
#include "editor/tool.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace apex::editor {

struct CompileOverlay {
    std::uint64_t revision = 0;
    std::filesystem::path workDirectory;
    std::vector<DVec3> leak;
    compile::PortalFile portals;
    bool showLeak = true;
    bool showPortals = true;
    std::optional<int> cluster; // draws only portals that touch this cluster

    std::optional<DVec3> leakStart() const { return leak.empty() ? std::nullopt : std::optional<DVec3>(leak.front()); }
};

// Reads `<name>.lin` and `<name>.prt` from `workDirectory`; a missing file is an empty part.
CompileOverlay loadCompileOverlay(const std::filesystem::path& workDirectory, const std::string& name,
                                  std::uint64_t revision);

// World-space primitives of the overlay; `stale` greys everything. Widths are logical pixels.
ToolOverlay compileOverlayPrimitives(const CompileOverlay& overlay, bool stale, bool leak, bool portals);

} // namespace apex::editor
