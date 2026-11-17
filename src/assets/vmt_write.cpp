#include "assets/vmt_write.hpp"

#include "assets/vmt.hpp"
#include "core/strings.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace apex::assets {
namespace {
bool validKey(std::string_view key) {
    return !key.empty() && std::ranges::none_of(key, [](unsigned char c) {
        return std::isspace(c) || c == '"' || c == '{' || c == '}' || c < 0x20;
    });
}
bool validValue(std::string_view value) {
    return std::ranges::none_of(value, [](unsigned char c) { return c < 0x20 || c == '"'; });
}
} // namespace

Result<std::string> editVmt(std::string_view source, std::span<const VmtEdit> edits) {
    for (const auto& edit : edits) {
        if (!validKey(edit.key))
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid VMT key"});
        if (edit.value && !validValue(*edit.value))
            return std::unexpected(Error{ErrorCode::InvalidArgument, "Invalid VMT value for " + edit.key});
    }
    auto parsed = kv1::parse(std::string(source), kv1::EscapeMode::Legacy);
    if (!parsed) return std::unexpected(parsed.error());
    auto children = parsed->root().children();
    if (children.size() != 1 || children[0].kind() != kv1::NodeKind::Block)
        return std::unexpected(Error{ErrorCode::ParseError, "A VMT is one shader block"});
    auto& shader = children[0];
    for (const auto& edit : edits) {
        std::optional<std::size_t> index;
        for (std::size_t i = 0; i < shader.children().size(); ++i) {
            const auto& child = shader.children()[i];
            if (child.kind() == kv1::NodeKind::Value && ciEquals(child.key(), edit.key)) {
                index = i;
                break;
            }
        }
        if (!edit.value) {
            if (index) {
                if (auto removed = shader.remove(*index); !removed) return std::unexpected(removed.error());
            }
        } else if (index) {
            if (auto set = shader.children()[*index].setValue(*edit.value); !set) return std::unexpected(set.error());
        } else if (auto added = shader.append(kv1::Node::value(edit.key, *edit.value)); !added) {
            return std::unexpected(added.error());
        }
    }
    return kv1::write(*parsed);
}

Result<std::filesystem::path> writeVmt(const Vfs& vfs, std::string_view material,
                                       std::span<const VmtEdit> edits) {
    const auto virtualPath = materialPath(material);
    if (!vfs.exists(virtualPath))
        return std::unexpected(Error{ErrorCode::NotFound, "No material " + std::string(material)});
    const auto path = vfs.loosePath(virtualPath);
    if (!path)
        return std::unexpected(Error{ErrorCode::Unsupported,
            "Material " + std::string(material) + " comes from " + vfs.displayPath(virtualPath) +
            ", an archive or plugin mount that cannot be written"});
    auto bytes = vfs.open(virtualPath);
    if (!bytes) return std::unexpected(bytes.error());
    auto edited = editVmt({reinterpret_cast<const char*>(bytes->data()), bytes->size()}, edits);
    if (!edited) return std::unexpected(edited.error());
    auto temporary = *path;
    temporary += ".apex-tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(edited->data(), static_cast<std::streamsize>(edited->size()));
        out.close();
        if (!out) {
            std::error_code ignore;
            std::filesystem::remove(temporary, ignore);
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + path->string()});
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, *path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return std::unexpected(Error{ErrorCode::IoError, "Cannot replace " + path->string()});
    }
    return path->lexically_normal();
}

} // namespace apex::assets
