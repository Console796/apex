#include "assets/addon_authoring.hpp"

#include "core/kv3.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>

namespace apex::assets {
namespace {
constexpr auto header = "<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->\n";
std::string quote(std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: if (static_cast<unsigned char>(c) >= 32) out += c; break;
        }
    }
    return out + '"';
}
Result<kv3::Document> read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected(Error{ErrorCode::IoError, "Cannot read " + file.string()});
    std::string bytes(std::istreambuf_iterator<char>(in), {});
    // Campaign scripts shipped by P2CE use headerless KV3; addon manifests retain the full header.
    if (file.filename() == "campaigns.kv3" && bytes.find("<!--") == std::string::npos)
        bytes.insert(0, header);
    return kv3::parse(bytes);
}
// Empty when portable across Windows and Linux; otherwise the reason.
std::string folderNameProblem(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return "is empty or a relative path component";
    if (name.find_first_of("/\\:*?\"<>|") != std::string::npos) return "contains one of / \\ : * ? \" < > |";
    for (const char c : name) if (static_cast<unsigned char>(c) < 32) return "contains a control character";
    if (name.back() == '.' || name.back() == ' ') return "ends with a dot or space";
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const std::set<std::string> reserved{"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (reserved.contains(stem)) return "is a reserved Windows device name";
    return {};
}
}
Result<void> createAddon(const std::filesystem::path& directory, const AddonScaffold& options) try {
    if (directory.empty() || options.name.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Choose an addon folder and display name"});
    if (const auto problem = folderNameProblem(directory.filename().string()); !problem.empty())
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Addon folder name \"" + directory.filename().string() + "\" " + problem});
    std::error_code ec;
    const auto parent = directory.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec))
        return std::unexpected(Error{ErrorCode::NotFound, "Parent folder does not exist: " + parent.string() + ". Create it or choose an existing folder."});
    if (std::filesystem::exists(std::filesystem::symlink_status(directory, ec)))
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Addon folder already exists: " + directory.string()});
    if (!std::filesystem::create_directory(directory, ec))
        return std::unexpected(Error{ec ? ErrorCode::IoError : ErrorCode::InvalidArgument,
            "Cannot create addon folder " + directory.string() + (ec ? ": " + ec.message() : std::string(": it already exists"))});
    struct Cleanup {
        std::filesystem::path path;
        bool keep = false;
        ~Cleanup() { if (!keep) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
    } cleanup{directory};
    for (const auto folder : {"mapsrc/instances", "maps", ".assets"})
        std::filesystem::create_directories(directory / folder);
    auto write = [&](const std::filesystem::path& file, const std::string& text) {
        std::ofstream out(file, std::ios::binary);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out << text;
        out.close();
    };
    write(directory / "addon.kv3", std::string(header) + "{\n mod = " + quote(options.name) +
        "\n description = \"\"\n type = " + quote(options.campaign ? "Campaign" : "Map") +
        "\n id = 0\n thumbnail = \"\"\n authors = []\n dependencies = []\n tags = []\n ignore = [\"mapsrc\"]\n metadata = {}\n assets = {}\n mounted_by_default = true\n}\n");
    if (options.campaign) {
        std::filesystem::create_directory(directory / "scripts");
        write(directory / "scripts/campaigns.kv3", std::string(header) +
            "{\n campaigns = {\n  main = {\n   title = " + quote(options.name) +
            "\n   chapters = []\n  }\n }\n}\n");
    }
    cleanup.keep = true;
    return {};
} catch (const std::exception& e) {
    return std::unexpected(Error{ErrorCode::IoError, e.what()});
}

Result<std::string> addonCampaign(const std::filesystem::path& directory, std::string_view id) try {
    auto manifest = read(directory / "addon.kv3");
    if (!manifest) return std::unexpected(manifest.error());
    const auto* name = manifest->root.find("mod");
    if (!name || !name->get<std::string>())
        return std::unexpected(Error{ErrorCode::ParseError, "Addon manifest needs a mod name"});
    const auto file = directory / "scripts/campaigns.kv3";
    if (!std::filesystem::exists(file)) {
        if (id.empty()) return std::string{};
        return std::unexpected(Error{ErrorCode::NotFound, "Campaigns file is missing: " + file.string() + " (requested campaign " + std::string(id) + ")"});
    }
    auto doc = read(file);
    if (!doc) return std::unexpected(doc.error());
    const auto* campaigns = doc->root.find("campaigns");
    const auto* object = campaigns ? campaigns->get<kv3::Value::Object>() : nullptr;
    if (!object) return std::unexpected(Error{ErrorCode::ParseError, "Campaigns must be an object"});
    std::string selected(id);
    if (selected.empty()) {
        if (object->empty()) return std::string{};
        if (object->size() != 1)
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Choose a campaign ID in the compile profile"});
        selected = object->front().key;
    }
    const auto* entry = campaigns->find(selected);
    if (!entry || !entry->get<kv3::Value::Object>() || selected.find_first_of("/\\") != std::string::npos)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Unknown campaign: " + selected});
    return "addon:" + std::filesystem::canonical(directory).filename().string() + "/" + selected;
} catch (const std::exception& e) {
    return std::unexpected(Error{ErrorCode::IoError, e.what()});
}

std::vector<std::string> addonMountDifference(const AddonList& editor, const AddonList& game) {
    const auto mounts = [](const AddonList& list) {
        std::vector<std::string> out;
        for (const auto& entry : list.entries) if (entry.mounted)
            out.push_back((entry.resolved.empty() ? std::filesystem::path(entry.dir) : entry.resolved).lexically_normal().generic_string());
        return out;
    };
    const auto a = mounts(editor), b = mounts(game);
    if (a == b) return {};
    std::vector<std::string> out;
    for (const auto& path : a) if (std::ranges::find(b, path) == b.end()) out.push_back("Editor only: " + path);
    for (const auto& path : b) if (std::ranges::find(a, path) == a.end()) out.push_back("Game only: " + path);
    if (out.empty()) out.push_back("Addon order differs between the editor and game");
    return out;
}
} // namespace apex::assets
