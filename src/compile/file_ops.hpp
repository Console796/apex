#pragma once

#include "compile/sequence.hpp"
#include "core/result.hpp"

#include <filesystem>
#include <span>

namespace apex::compile {

class FileOps {
public:
    virtual ~FileOps() = default;
    virtual Result<void> copy(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
    virtual Result<void> remove(const std::filesystem::path& path) = 0;
    virtual Result<void> rename(const std::filesystem::path& from, const std::filesystem::path& to) = 0;
    virtual bool exists(const std::filesystem::path& path) const = 0;
};

class FilesystemFileOps final : public FileOps {
public:
    Result<void> copy(const std::filesystem::path& from, const std::filesystem::path& to) override;
    Result<void> remove(const std::filesystem::path& path) override;
    Result<void> rename(const std::filesystem::path& from, const std::filesystem::path& to) override;
    bool exists(const std::filesystem::path& path) const override;
};

Result<void> runSpecialCommand(SpecialCommand command, std::span<const std::string> arguments,
                               std::filesystem::path& cwd,
                               std::span<const std::filesystem::path> allowedRoots, FileOps& files);

} // namespace apex::compile
