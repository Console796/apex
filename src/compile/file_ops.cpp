#include "compile/file_ops.hpp"

namespace apex::compile {
namespace {
Result<void> result(const std::error_code& ec) {
    if (ec) return std::unexpected(Error{ErrorCode::IoError, ec.message()});
    return {};
}
}

Result<void> FilesystemFileOps::copy(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (!to.parent_path().empty()) std::filesystem::create_directories(to.parent_path(), ec);
    if (ec) return result(ec);
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    return result(ec);
}
Result<void> FilesystemFileOps::remove(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return result(ec);
}
Result<void> FilesystemFileOps::rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return result(ec);
}
bool FilesystemFileOps::exists(const std::filesystem::path& path) const {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

} // namespace apex::compile
