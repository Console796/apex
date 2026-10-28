#include "editor/compile_snapshot.hpp"

#include <fstream>

namespace apex::editor {

Result<CompileSnapshot> writeCompileSnapshot(const document::Document& document,
    const std::filesystem::path& source, const std::filesystem::path& cache,
    const CompileGamePaths& game) {
    auto text = document.write();
    if (!text) return std::unexpected(text.error());
    try {
        auto name = source.empty() ? std::string("untitled") : source.stem().string();
        if (name.empty() || name == "." || name == "..") name = "untitled";
        CompileSnapshot snapshot;
        snapshot.revision = document.historyState();
        auto& paths = snapshot.paths;
        paths.workDirectory = std::filesystem::absolute(cache / "compile" / name).lexically_normal();
        // Reject a source inside the work directory before removing any previous outputs.
        const auto relative = std::filesystem::weakly_canonical(source).lexically_relative(
            std::filesystem::weakly_canonical(paths.workDirectory));
        if (!source.empty() && !relative.empty() && *relative.begin() != "..")
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Compile work directory contains the source map"});
        // Do not follow a replaced work-directory symlink when cleaning outputs.
        if (std::filesystem::is_symlink(paths.workDirectory) ||
            std::filesystem::is_symlink(paths.workDirectory.parent_path()))
            return std::unexpected(Error{ErrorCode::IoError, "Compile directory is a symlink"});
        std::filesystem::create_directories(paths.workDirectory);
        for (const auto& entry : std::filesystem::directory_iterator(paths.workDirectory))
            std::filesystem::remove_all(entry.path());
        snapshot.path = paths.workDirectory / (name + ".vmf");
        std::ofstream out(snapshot.path, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.write(text->data(), static_cast<std::streamsize>(text->size()));
        out.close();
        paths.bspDirectory = game.gameDirectory / "maps";
        paths.toolDirectory = game.toolDirectory;
        paths.instanceRoot = game.instanceRoot;
        paths.variables = game.tools;
        paths.variables["path"] = paths.workDirectory.string();
        paths.variables["file"] = name;
        paths.variables["mapname"] = name;
        paths.variables["work"] = paths.workDirectory.string();
        paths.variables["addon"] = paths.addonDirectory.string();
        paths.variables["gamedir"] = game.gameDirectory.string();
        paths.variables["bspdir"] = paths.bspDirectory.string();
        return snapshot;
    } catch (const std::exception& error) {
        return std::unexpected(Error{ErrorCode::IoError, error.what()});
    }
}

} // namespace apex::editor
