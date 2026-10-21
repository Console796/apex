#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
struct GameTree {
    std::filesystem::path root;
    GameTree() {
        static std::atomic<unsigned> serial{0};
        root = std::filesystem::temp_directory_path() /
               ("apex-vfs-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(serial++));
        std::filesystem::create_directories(root);
    }
    ~GameTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void put(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories((root / path).parent_path());
        std::ofstream file(root / path, std::ios::binary);
        file << text;
        if (!file) {
            throw std::runtime_error("Cannot create fixture");
        }
    }
};
} // namespace
