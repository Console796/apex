#include "editor/compile_snapshot.hpp"

#include "document/cordon.hpp"
#include "editor/entity_defaults.hpp"

#include <algorithm>

#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace apex::editor {
namespace {

Result<std::string> lightDefaults(std::string text, const CompileGamePaths& game) {
    if (!game.classes) return text;
    auto parsed = kv1::parse(text, game.escapeMode);
    if (!parsed) return std::unexpected(parsed.error());
    bool changed = false;
    const auto visit = [&](const auto& self, kv1::Node& block) -> Result<void> {
        if (ciEquals(block.key(), "entity")) {
            const auto* name = block.find("classname");
            const auto* cls = name ? game.classes->find(name->decodedValue().value_or("")) : nullptr;
            // Existing non-light entities retain their original omission semantics. For lights,
            // compile exactly the effective values shown by the inspector, preserving explicit zeros.
            if (cls && std::ranges::any_of(cls->properties, [](const auto& p) { return ciEquals(p.name, "_light"); })) {
                for (const auto& [key, value] : missingEntityDefaults(block, *cls)) {
                    auto appended = block.append(kv1::Node::value(key, value));
                    if (!appended) return std::unexpected(appended.error());
                    changed = true;
                }
            }
        }
        for (auto& child : block.children()) {
            if (child.kind() != kv1::NodeKind::Block) continue;
            auto result = self(self, child);
            if (!result) return result;
        }
        return {};
    };
    auto result = visit(visit, parsed->root());
    if (!result) return std::unexpected(result.error());
    return changed ? kv1::write(*parsed) : Result<std::string>{std::move(text)};
}

// vbsp resolves an instance `file` from the VMF's folder, then from the one -instancepath. The snapshot sits in
// a cache folder, so instances that exist only beside the source map are copied in, keeping their relative path.
Result<void> copySourceInstances(std::string& text, const std::filesystem::path& sourceDirectory,
    const std::filesystem::path& work, const CompileGamePaths& game) {
    namespace fs = std::filesystem;
    std::map<fs::path, fs::path> copied;
    std::set<fs::path> destinations;
    const auto rewrite = [&](const auto& self, std::string& body, const fs::path& sourceFolder,
                             const fs::path& destinationFolder) -> Result<void> {
        auto parsed = kv1::parse(body, game.escapeMode);
        if (!parsed) return std::unexpected(parsed.error());
        bool changed = false;
        for (auto& entity : parsed->root().children()) {
            const auto* cls = entity.find("classname");
            auto* file = entity.find("file");
            if (!cls || !ciEquals(cls->decodedValue().value_or(""), "func_instance") || !file) continue;
            std::string spelling = file->decodedValue().value_or("");
            std::ranges::replace(spelling, '\\', '/');
            const fs::path reference(spelling);
            // Preserve SDK references that aren't source-relative. vbsp resolves them through -instancepath.
            fs::path from;
            for (const auto& candidate : {sourceFolder / reference, sourceDirectory / reference})
                if (fs::is_regular_file(candidate)) { from = fs::weakly_canonical(candidate); break; }
            if (from.empty()) continue;
            auto existing = copied.find(from);
            fs::path destination;
            bool fresh = existing == copied.end();
            if (!fresh) destination = existing->second;
            else {
                auto relative = from.lexically_relative(fs::weakly_canonical(sourceDirectory));
                if (relative.empty() || *relative.begin() == ".." || destinations.contains(relative))
                    relative = fs::path("__apex_instances") / std::to_string(copied.size()) / from.filename();
                destination = work / relative;
                copied[from] = destination;
                destinations.insert(relative);
            }
            if (fresh) {
                std::ifstream in(from, std::ios::binary);
                in.exceptions(std::ios::failbit | std::ios::badbit);
                auto instance = lightDefaults(std::string(std::istreambuf_iterator<char>(in), {}), game);
                if (!instance) return std::unexpected(instance.error().withContext(from.string()));
                auto nested = self(self, *instance, from.parent_path(), destination.parent_path());
                if (!nested) return nested;
                fs::create_directories(destination.parent_path());
                std::ofstream out(destination, std::ios::binary);
                out.exceptions(std::ios::failbit | std::ios::badbit);
                out << *instance;
                out.close();
            }
            const auto replacement = destination.lexically_relative(destinationFolder).generic_string();
            if (replacement != spelling) { file->setValue(replacement).value(); changed = true; }
        }
        if (changed) {
            auto written = kv1::write(*parsed);
            if (!written) return std::unexpected(written.error());
            body = std::move(*written);
        }
        return {};
    };
    return rewrite(rewrite, text, sourceDirectory, work);
}

} // namespace

Result<CompileSnapshot> writeCompileSnapshot(const document::Document& document,
    const std::filesystem::path& source, const std::filesystem::path& cache,
    const CompileGamePaths& game, const std::optional<CompileCordon>& cordon) {
    auto text = document.write();
    if (!text) return std::unexpected(text.error());
    text = lightDefaults(std::move(*text), game);
    if (!text) return std::unexpected(text.error());
    if (cordon) {
        const auto boxes = document::readCordons(document).compileBoxes();
        if (!boxes.empty()) {
            auto filtered = document::cordonFilteredText(*text, boxes, {.material = cordon->material});
            if (!filtered) return std::unexpected(filtered.error());
            *text = std::move(*filtered);
        }
    }
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
        if (!source.empty()) {
            auto copied = copySourceInstances(*text, source.parent_path(), paths.workDirectory, game);
            if (!copied) return std::unexpected(copied.error());
        }
        snapshot.path = paths.workDirectory / (name + ".vmf");
        std::ofstream out(snapshot.path, std::ios::binary | std::ios::trunc);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.write(text->data(), static_cast<std::streamsize>(text->size()));
        out.close();
        if (game.contentGameInfo) {
            paths.contentGameDirectory = paths.workDirectory / "content";
            std::filesystem::create_directories(paths.contentGameDirectory);
            std::ofstream info(paths.contentGameDirectory / "gameinfo.txt", std::ios::binary);
            info.exceptions(std::ios::failbit | std::ios::badbit);
            info << *game.contentGameInfo;
        }
        paths.addonDirectory = game.addonDirectory;
        paths.bspDirectory = (game.addonDirectory.empty() ? game.gameDirectory : game.addonDirectory) / "maps";
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
