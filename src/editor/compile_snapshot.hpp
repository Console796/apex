#pragma once

#include "compile/runner.hpp"
#include "document/document.hpp"

namespace apex::editor {

// GameProfile paths are resolved by the caller, keeping assets out of the editor layer.
struct CompileGamePaths {
    std::filesystem::path gameDirectory;
    std::filesystem::path toolDirectory;
    std::filesystem::path instanceRoot;
    compile::Variables tools;
};

struct CompileSnapshot {
    std::filesystem::path path;
    compile::CompilePaths paths;
    std::uint64_t revision = 0;
};

// A cordon compile (D7): the snapshot holds only what touches an active cordon, closed by six walls.
struct CompileCordon {
    std::string material = "tools/toolsskybox";
};

// Call on the document's owning thread at Run. Never saves or modifies the source document.
Result<CompileSnapshot> writeCompileSnapshot(const document::Document& document,
    const std::filesystem::path& source, const std::filesystem::path& cache,
    const CompileGamePaths& game, const std::optional<CompileCordon>& cordon = std::nullopt);

} // namespace apex::editor
