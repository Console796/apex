#include "compile/map_files.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace apex::compile {
namespace {
std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.push_back(line);
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return out;
}

// Reads `count` numbers; false when fewer are present or a token is not a number.
bool numbers(std::istringstream& in, double* out, int count) {
    for (int i = 0; i < count; ++i) {
        std::string token;
        if (!(in >> token)) return false;
        char* end = nullptr;
        out[i] = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end) return false;
    }
    return true;
}

std::optional<std::string> slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), {});
}
}

std::vector<DVec3> parseLeakFile(std::string_view text) {
    std::vector<DVec3> points;
    for (const auto line : lines(text)) {
        std::istringstream in{std::string(line)};
        double v[3];
        if (!numbers(in, v, 3)) break;
        points.emplace_back(v[0], v[1], v[2]);
    }
    return points;
}

PortalFile parsePortalFile(std::string_view text) {
    PortalFile file;
    const auto all = lines(text);
    if (all.size() < 3 || all[0] != "PRT1") return file;
    int declared = 0;
    {
        std::istringstream clusters{std::string(all[1])}, portals{std::string(all[2])};
        double c = 0, p = 0;
        if (!numbers(clusters, &c, 1) || !numbers(portals, &p, 1) || c < 0 || p < 0) return file;
        file.clusters = static_cast<int>(c);
        declared = static_cast<int>(p);
    }
    for (std::size_t i = 3; i < all.size() && static_cast<int>(file.portals.size()) < declared; ++i) {
        std::string cleaned(all[i]);
        for (auto& c : cleaned) if (c == '(' || c == ')') c = ' ';
        std::istringstream in(cleaned);
        double header[3];
        if (!numbers(in, header, 3) || header[0] < 3 || header[0] > 256) break;
        PortalPolygon polygon;
        polygon.clusterA = static_cast<int>(header[1]);
        polygon.clusterB = static_cast<int>(header[2]);
        bool complete = true;
        for (int k = 0; k < static_cast<int>(header[0]); ++k) {
            double v[3];
            if (!numbers(in, v, 3)) { complete = false; break; }
            polygon.points.emplace_back(v[0], v[1], v[2]);
        }
        if (!complete) break;
        file.portals.push_back(std::move(polygon));
    }
    return file;
}

std::optional<std::vector<DVec3>> readLeakFile(const std::filesystem::path& directory, const std::string& name) {
    const auto text = slurp(directory / (name + ".lin"));
    if (!text) return std::nullopt;
    return parseLeakFile(*text);
}

std::optional<PortalFile> readPortalFile(const std::filesystem::path& directory, const std::string& name) {
    const auto text = slurp(directory / (name + ".prt"));
    if (!text) return std::nullopt;
    return parsePortalFile(*text);
}

} // namespace apex::compile
