#include "compile/file_ops.hpp"

#include <algorithm>

namespace apex::compile {
namespace {
Result<std::filesystem::path> checkedPath(const std::filesystem::path& path,
                                         const std::filesystem::path& cwd,
                                         std::span<const std::filesystem::path> roots) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path.is_absolute() ? path : cwd / path, ec).lexically_normal();
    if (ec) return std::unexpected(Error{ErrorCode::IoError, ec.message()});
    const auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    if (ec) return std::unexpected(Error{ErrorCode::IoError, ec.message()});
    for (const auto& root : roots) {
        if (root.empty()) continue;
        const auto base = std::filesystem::weakly_canonical(std::filesystem::absolute(root, ec), ec);
        if (ec) return std::unexpected(Error{ErrorCode::IoError, ec.message()});
        const auto match = std::mismatch(base.begin(), base.end(), canonical.begin(), canonical.end());
        if (match.first == base.end()) return canonical;
    }
    return std::unexpected(Error{ErrorCode::InvalidArgument, "Compile path outside allowed roots: " + absolute.string()});
}
}

Result<void> runSpecialCommand(SpecialCommand command, std::span<const std::string> arguments,
                               std::filesystem::path& cwd,
                               std::span<const std::filesystem::path> allowedRoots, FileOps& files) {
    const bool pair = command == SpecialCommand::Copy || command == SpecialCommand::Rename ||
                      command == SpecialCommand::CopyIfExists;
    if (arguments.size() != (pair ? 2u : 1u)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "Wrong special command argument count"});
    }
    auto from = checkedPath(arguments[0], cwd, allowedRoots);
    if (!from) return std::unexpected(from.error());
    std::filesystem::path to;
    if (pair) {
        auto destination = checkedPath(arguments[1], cwd, allowedRoots);
        if (!destination) return std::unexpected(destination.error());
        to = *destination;
    }
    switch (command) {
    case SpecialCommand::ChangeDirectory: {
        std::error_code ec;
        if (!std::filesystem::is_directory(*from, ec))
            return std::unexpected(Error{ErrorCode::NotFound, "Compile directory not found: " + from->string()});
        cwd = *from;
        return {};
    }
    case SpecialCommand::Copy: return files.copy(*from, to);
    case SpecialCommand::Delete: return files.remove(*from);
    case SpecialCommand::Rename: return files.rename(*from, to);
    case SpecialCommand::CopyIfExists: return files.exists(*from) ? files.copy(*from, to) : Result<void>{};
    }
    return std::unexpected(Error{ErrorCode::Unsupported, "Unknown special command"});
}

} // namespace apex::compile
