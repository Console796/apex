#include "editor/editor_session.hpp"

#include "document/connection_codec.hpp"
#include "editor/instance_templates.hpp"
#include "editor/prefab.hpp"

#include <cstdio>
#include <iomanip>
#include <set>
#include <sstream>

namespace apex::editor {
namespace {
std::string value(const kv1::Node& block, std::string_view key) {
    const auto* node = block.find(key);
    return node ? node->decodedValue().value_or("") : "";
}
void walk(const document::Document& doc, const std::function<void(NodeId, const document::Node&)>& visit) {
    const auto recurse = [&](const auto& self, NodeId id) -> void {
        const auto* node = doc.get(*doc.handle(id));
        visit(id, *node);
        for (const auto child : node->children) self(self, child);
    };
    for (const auto root : doc.roots()) recurse(recurse, root);
}
}

std::vector<std::string> EditorSession::instanceBoundaryConnections() const {
    auto fragment = copyText();
    if (!document_ || !fragment) return {};
    auto parsed = kv1::parse(*fragment);
    if (!parsed) return {};
    std::set<std::string> selected;
    for (const auto& child : parsed->root().children())
        if (ciEquals(child.key(), "entity")) selected.insert(value(child, "id"));
    std::vector<std::string> warnings;
    walk(*document_, [&](NodeId nodeId, const document::Node& node) {
        if (!node.block || !ciEquals(node.block->key(), "entity")) return;
        const bool inside = selected.contains(value(*node.block, "id"));
        const auto exported = document_->exportSubtree(nodeId);
        const auto* connections = exported.find("connections");
        if (!connections) return;
        for (const auto& entry : connections->children()) {
            const auto connection = document::parseConnection(entry.decodedValue().value_or(""));
            bool crosses = false;
            const auto targets = names_->matching(connection.target);
            for (const auto target : targets) {
                const auto* destination = document_->get(*document_->handle(target));
                crosses |= selected.contains(value(*destination->block, "id")) != inside;
            }
            // Unresolved outgoing names may refer to entities outside the selection or a parent instance.
            if (inside && targets.empty() && !connection.target.empty() && connection.target.front() != '!') crosses = true;
            if (crosses) warnings.push_back(value(*node.block, "targetname") + " (#" + value(*node.block, "id") +
                ") " + std::string(entry.key()) + " -> " + connection.target);
        }
    });
    return warnings;
}

Result<void> EditorSession::createInstanceFromSelection(const std::filesystem::path& file,
    std::string reference, bool allowCrossingConnections) {
    const auto invalid = [](std::string message) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
    };
    if (!document_) return invalid("No document");
    if (file.empty() || !ciEquals(file.extension().string(), ".vmf") || reference.empty())
        return invalid("Choose a new VMF file and an instance reference");
    if (!allowCrossingConnections && !instanceBoundaryConnections().empty())
        return invalid("Connections cross the selection boundary; review them before creating the instance");
    auto fragment = copyText();
    if (!fragment) return std::unexpected(fragment.error());
    auto map = prefabMapText(*fragment);
    if (!map) return std::unexpected(map.error());
    auto parsed = kv1::parse(*map);
    if (!parsed) return std::unexpected(parsed.error());
    if (auto* info = parsed->root().find("versioninfo"))
        if (auto* prefab = info->find("prefab")) *prefab = kv1::Node::value("prefab", "0");
    std::error_code absoluteError;
    const auto destination = std::filesystem::absolute(file, absoluteError).lexically_normal();
    if (absoluteError) return std::unexpected(Error{ErrorCode::IoError, absoluteError.message()});
    // The resolver walks real folders, so a rebased "../" needs the new file's folder to exist.
    // Folders made here are removed again (deepest first, only while empty) if nothing is written.
    struct Folders {
        std::vector<std::filesystem::path> created;
        bool keep = false;
        ~Folders() { if (!keep) for (const auto& folder : created) { std::error_code error; std::filesystem::remove(folder, error); } }
    } folders;
    for (auto folder = destination.parent_path(); !folder.empty() && !std::filesystem::exists(folder, absoluteError);
         folder = folder.parent_path()) folders.created.push_back(folder);
    std::filesystem::create_directories(destination.parent_path(), absoluteError);
    if (absoluteError) return std::unexpected(Error{ErrorCode::IoError, absoluteError.message()});
    // A spelling keeps its meaning only if it finds the same template from the new file, and not the
    // new file itself (it does not exist yet, so the resolver cannot see that collision).
    const auto sameTarget = [&](const std::string& spelling, const std::filesystem::path& target) {
        return !ciEquals((destination.parent_path() / spelling).lexically_normal().generic_string(),
                         destination.generic_string()) &&
               resolveInstanceFile(destination, spelling, connectionExtraRoot_) == target;
    };
    const auto rebase = [&](const auto& self, kv1::Node& block) -> Result<void> {
        if (ciEquals(block.key(), "entity") && ciEquals(value(block, "classname"), "func_instance")) {
            const auto nestedFile = value(block, "file");
            const auto resolved = resolveInstanceFile(connectionMapPath_, nestedFile, connectionExtraRoot_);
            if (resolved && !sameTarget(nestedFile, *resolved)) {
                std::error_code ec;
                const auto relative = std::filesystem::relative(*resolved, destination.parent_path(), ec).generic_string();
                // Another drive, or a folder outside the instance roots, leaves no spelling that works.
                if (ec || relative.empty() || !sameTarget(relative, *resolved))
                    return invalid("Nested instance \"" + nestedFile + "\" would not resolve from " + destination.string() +
                        "; save the new instance under the map's maps folder or the instance search root");
                *block.find("file") = kv1::Node::value("file", relative);
            }
        }
        for (auto& child : block.children()) {
            auto result = self(self, child);
            if (!result) return result;
        }
        return {};
    };
    auto rebased = rebase(rebase, parsed->root());
    if (!rebased) return std::unexpected(rebased.error());
    auto localText = kv1::write(*parsed);
    if (!localText) return std::unexpected(localText.error());
    auto localFile = vmf::parse(*localText);
    if (!localFile) return std::unexpected(localFile.error());
    document::Document local(*localFile);
    std::vector<NodeId> objects;
    walk(local, [&](NodeId id, const document::Node& node) {
        if (node.block && (ciEquals(node.block->key(), "entity") || ciEquals(node.block->key(), "solid"))) objects.push_back(id);
    });
    const auto context = gizmoContext();
    if (!context) return invalid("The selection has no pivot");
    const auto pivot = context->pivot;
    auto moved = commitTransform(local, objects, Transform::translate(-pivot), {true, &entityClasses_}, "Recenter instance");
    if (!moved) return std::unexpected(moved.error());
    auto text = local.write();
    if (!text) return std::unexpected(text.error());
#ifdef _WIN32
    auto* output = _wfopen(file.c_str(), L"wbx");
#else
    auto* output = std::fopen(file.c_str(), "wbx");
#endif
    if (!output) return std::unexpected(Error{ErrorCode::IoError, "Cannot create a new instance file: " + file.string()});
    folders.keep = true;
    const bool written = std::fwrite(text->data(), 1, text->size(), output) == text->size();
    const bool closed = std::fclose(output) == 0;
    struct Cleanup {
        std::filesystem::path file;
        bool keep = false;
        ~Cleanup() { if (!keep) { std::error_code error; std::filesystem::remove(file, error); } }
    } cleanup{file};
    if (!written || !closed) return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + file.string()});
    kv1::Document replacement;
    auto entity = kv1::Node::block("entity");
    std::ostringstream origin;
    origin << std::setprecision(17) << pivot.x << ' ' << pivot.y << ' ' << pivot.z;
    for (const auto& [key, val] : std::vector<std::pair<std::string, std::string>>{
             {"id", "1"}, {"classname", "func_instance"}, {"file", std::move(reference)},
             {"origin", origin.str()}, {"angles", "0 0 0"}, {"fixup_style", "2"}})
        entity.append(kv1::Node::value(key, val)).value();
    replacement.root().append(std::move(entity)).value();
    const auto replacementText = kv1::write(replacement);
    if (!replacementText) return std::unexpected(replacementText.error());
    auto result = runEdit("Create instance from selection", AfterEdit::Computed, [&](document::Transaction& tx) -> Result<EditedObjects> {
        auto removed = stageDelete(*document_, *groups_, selection_, tx);
        if (!removed) return std::unexpected(removed.error());
        PasteOptions options;
        options.inPlace = true;
        options.classes = &entityClasses_;
        return stagePaste(*document_, tx, *replacementText, options);
    });
    cleanup.keep = result.has_value();
    // Keep the external VMF on undo so redo and other maps referring to it remain valid.
    return result;
}
} // namespace apex::editor
