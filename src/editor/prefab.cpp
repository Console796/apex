#include "editor/prefab.hpp"

#include "core/kv1.hpp"
#include "core/strings.hpp"
#include "editor/clipboard.hpp"
#include "editor/editor_session.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <tuple>

namespace apex::editor {
namespace {

using Kv = kv1::Node;

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message)}; }
Error ioError(std::string message) { return Error{ErrorCode::IoError, std::move(message)}; }

bool isBlock(const Kv& node, std::string_view key) {
    return node.kind() == kv1::NodeKind::Block && ciEquals(node.key(), key);
}

// A single path component that is safe on Windows and Linux.
bool plainComponent(std::string_view part) {
    if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
    return part.find_first_of("<>:\"/\\|?*") == std::string_view::npos &&
           std::ranges::none_of(part, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

int idOf(const Kv& node) {
    const auto* id = node.find("id");
    const auto value = id ? id->asInt() : Result<int>(std::unexpected(Error{ErrorCode::NotFound, ""}));
    return value ? *value : 0;
}

void maxId(const Kv& block, int& out) {
    out = std::max(out, idOf(block));
    for (const auto& child : block.children()) {
        if (child.kind() == kv1::NodeKind::Block) maxId(child, out);
    }
}

} // namespace

std::filesystem::path prefabFolder(const std::filesystem::path& gameDirectory, const std::filesystem::path& userFolder) {
    if (!userFolder.empty()) return userFolder;
    return gameDirectory.parent_path() / "hammer" / "prefabs";
}

std::vector<PrefabEntry> listPrefabs(const std::filesystem::path& folder) {
    std::vector<PrefabEntry> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return out;
    for (std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || !ciEquals(it->path().extension().string(), ".vmf")) continue;
        PrefabEntry entry;
        entry.file = it->path();
        entry.name = it->path().stem().string();
        entry.category = it->path().parent_path().lexically_relative(folder).generic_string();
        if (entry.category == ".") entry.category.clear();
        out.push_back(std::move(entry));
    }
    std::ranges::sort(out, [](const PrefabEntry& a, const PrefabEntry& b) {
        return std::tie(a.category, a.name) < std::tie(b.category, b.name);
    });
    return out;
}

Result<std::string> prefabMapText(std::string_view fragment) {
    auto parsed = kv1::parse(std::string(fragment));
    if (!parsed) return std::unexpected(parsed.error().withContext("prefab fragment is not VMF text"));
    kv1::Document out;
    auto info = Kv::block("versioninfo");
    for (const auto& [key, value] : {std::pair{"editorversion", "400"}, {"editorbuild", "0"}, {"mapversion", "1"},
                                     {"formatversion", "100"}, {"prefab", "1"}})
        (void)info.append(Kv::value(key, value));
    (void)out.root().append(std::move(info));

    int highest = 0;
    std::vector<Kv> worldChildren, entities;
    for (const auto& child : parsed->root().children()) {
        if (child.kind() != kv1::NodeKind::Block) continue;
        if (isBlock(child, "visgroups")) {
            (void)out.root().append(child);
        } else if (isBlock(child, "solid") || isBlock(child, "group")) {
            maxId(child, highest);
            worldChildren.push_back(child);
        } else if (isBlock(child, "entity")) {
            maxId(child, highest);
            entities.push_back(child);
        }
    }
    if (worldChildren.empty() && entities.empty()) return std::unexpected(invalid("the prefab holds no solids, entities or groups"));

    auto world = Kv::block("world");
    (void)world.append(Kv::value("id", std::to_string(highest + 1)));
    (void)world.append(Kv::value("mapversion", "1"));
    (void)world.append(Kv::value("classname", "worldspawn"));
    for (auto& child : worldChildren) (void)world.append(std::move(child));
    (void)out.root().append(std::move(world));
    for (auto& entity : entities) (void)out.root().append(std::move(entity));
    return kv1::write(out);
}

Result<std::string> prefabFragment(std::string_view mapText) {
    auto parsed = kv1::parse(std::string(mapText));
    if (!parsed) return std::unexpected(parsed.error().withContext("prefab is not VMF text"));
    kv1::Document out;
    for (const auto& child : parsed->root().children()) {
        if (child.kind() != kv1::NodeKind::Block) continue;
        if (isBlock(child, "visgroups") || isBlock(child, "entity") || isBlock(child, "solid") || isBlock(child, "group")) {
            (void)out.root().append(child);
        } else if (isBlock(child, "world")) {
            for (const auto& member : child.children()) {
                if (isBlock(member, "solid") || isBlock(member, "group")) (void)out.root().append(member);
            }
        }
    }
    return kv1::write(out);
}

Result<std::filesystem::path> createPrefab(const EditorSession& session, std::string_view name,
                                           std::string_view category, const std::filesystem::path& folder,
                                           bool overwrite) {
    if (folder.empty()) return std::unexpected(invalid("no prefab folder"));
    if (!plainComponent(name)) return std::unexpected(invalid("the prefab name is not a valid file name"));
    std::filesystem::path directory = folder;
    for (const auto& part : split(category, '/')) {
        if (part.empty()) continue;
        if (!plainComponent(part)) return std::unexpected(invalid("the prefab category is not a valid folder name"));
        directory /= std::string(part);
    }
    auto fragment = session.copyText();
    if (!fragment) return std::unexpected(fragment.error());
    auto text = prefabMapText(*fragment);
    if (!text) return std::unexpected(text.error());

    const auto file = directory / (std::string(name) + ".vmf");
    std::error_code ec;
    if (!overwrite && std::filesystem::exists(file, ec)) return std::unexpected(invalid("a prefab named that already exists"));
    std::filesystem::create_directories(directory, ec);
    if (ec) return std::unexpected(ioError("cannot create " + directory.string() + ": " + ec.message()));
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << *text;
    stream.close();
    if (!stream) return std::unexpected(ioError("cannot write " + file.string()));
    return file;
}

Result<void> placePrefabText(EditorSession& session, std::string_view fileText, const PrefabPlacement& where) {
    auto fragment = prefabFragment(fileText);
    if (!fragment) return std::unexpected(fragment.error());
    PasteOptions options;
    options.at = where.at;
    options.plane = where.plane.value_or(SurfacePlane{where.at, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {}});
    options.snap = where.snap;
    return session.pasteText(*fragment, std::move(options));
}

Result<void> placePrefab(EditorSession& session, const std::filesystem::path& file, const PrefabPlacement& where) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::unexpected(Error{ErrorCode::NotFound, "cannot open " + file.string()});
    std::ostringstream text;
    text << stream.rdbuf();
    return placePrefabText(session, text.str(), where);
}

Result<FragmentShape> prefabShape(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return std::unexpected(Error{ErrorCode::NotFound, "cannot open " + file.string()});
    std::ostringstream text;
    text << stream.rdbuf();
    auto fragment = prefabFragment(text.str());
    if (!fragment) return std::unexpected(fragment.error());
    return fragmentShape(*fragment);
}

AABB prefabLocalBounds(const AABB& bounds) {
    const DVec3 half = (bounds.max - bounds.min) * 0.5;
    return AABB{{-half.x, -half.y, 0.0}, {half.x, half.y, half.z * 2.0}};
}

PrefabItem prefabItem(const std::filesystem::path& file) {
    PrefabItem item;
    item.path = file.string();
    if (const auto shape = prefabShape(file); shape && !shape->bounds.empty()) item.bounds = prefabLocalBounds(shape->bounds);
    item.place = [file](EditorSession& session, const EntityPlacement& placement) {
        return placePrefab(session, file, PrefabPlacement{placement.origin, std::nullopt, std::nullopt});
    };
    return item;
}

} // namespace apex::editor
