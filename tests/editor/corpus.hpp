#pragma once

#include "fixtures.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace editor_test {

inline std::vector<std::filesystem::path> corpusPaths() {
    const char* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) return {};
    const auto root = std::filesystem::path(install) / "sdk_content/maps";
    REQUIRE(std::filesystem::is_directory(root));
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    REQUIRE_FALSE(paths.empty());
    return paths;
}

inline vmf::VmfFile readCorpus(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    auto file = vmf::parse(bytes);
    if (!file) file = vmf::parse(bytes, {kv1::EscapeMode::Legacy});
    REQUIRE(file);
    return std::move(*file);
}

} // namespace editor_test
