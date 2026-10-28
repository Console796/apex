#include "corpus.hpp"
#include "assets/vmt.hpp"
#include "core/paths.hpp"
#include "core/strings.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <sourcepp/Math.h>
#include <steampp/steampp.h>
#include <vpkpp/format/VPK.h>

using namespace editor_test;

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string text(const kv1::Node& node, std::string_view key) {
    const auto* value = node.find(key);
    return value ? value->decodedValue().value_or("") : "";
}

kv1::Document readKv(const fs::path& path, kv1::EscapeMode mode) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    auto parsed = kv1::parse({std::istreambuf_iterator<char>{input}, {}}, mode);
    REQUIRE(parsed);
    return std::move(*parsed);
}

bool active(const kv1::Node& node) {
    const auto condition = toLowerAscii(node.condition());
#ifdef _WIN32
    return condition.empty() || condition == "[$win32]" || condition == "[!$linux]" || condition == "[!$posix]";
#else
    return condition.empty() || condition == "[$linux]" || condition == "[$posix]" || condition == "[!$win32]";
#endif
}

bool flag(const kv1::Node& node, std::string_view key, bool fallback = false) {
    const auto* value = node.find(key);
    return value ? value->decodedValue().value_or("") == "1" : fallback;
}

bool matches(std::string_view pattern, std::string_view value) {
    std::size_t p = 0, v = 0, star = std::string_view::npos, retry = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = v;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            v = ++retry;
        } else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// Vfs keeps its mounts private. Mirror its search-path expansion locally for this probe.
std::vector<fs::path> expand(const fs::path& requested) {
    std::vector<fs::path> paths{requested.is_absolute() ? requested.root_path() : fs::path{"."}};
    for (const auto& component : requested.relative_path()) {
        std::vector<fs::path> next;
        for (const auto& parent : paths) {
            if (component == "." || component == "..") {
                next.push_back(parent / component);
                continue;
            }
            if (!fs::is_directory(parent)) continue;
            for (const auto& entry : fs::directory_iterator(parent))
                if (matches(toLowerAscii(component.string()), toLowerAscii(entry.path().filename().string())))
                    next.push_back(entry.path());
        }
        std::ranges::sort(next);
        paths = std::move(next);
    }
    return paths;
}

std::vector<fs::path> mountRequests(const assets::GameProfile& profile) {
    const auto gameDir = fs::absolute(profile.gameDirectory);
    const auto engineDir = gameDir.parent_path();
    const auto gameinfo = readKv(gameDir / "gameinfo.txt", profile.escapeMode);
    const auto* info = gameinfo.root().find("GameInfo");
    REQUIRE(info);
    const auto* filesystem = info->find("FileSystem");
    REQUIRE(filesystem);
    const auto* search = filesystem->find("SearchPaths");
    REQUIRE(search);
    std::vector<fs::path> paths;
    for (const auto& node : search->children()) {
        bool game = false;
        for (const auto id : split(node.key(), '+')) game |= ciEquals(id, "game");
        if (!game || !active(node)) continue;
        auto path = normalizePath(node.decodedValue().value());
        for (const auto& [token, replacement] :
             {std::pair{"|gameinfo_path|", gameDir.generic_string() + '/'},
              std::pair{"|all_source_engine_paths|", engineDir.generic_string() + '/'}})
            if (toLowerAscii(path).starts_with(token))
                path.replace(0, std::char_traits<char>::length(token), replacement);
        paths.push_back(engineDir / path);
    }
    const auto mountFiles = expand(gameDir / "cfg/mounts.kv");
    if (mountFiles.empty()) return paths;
    const auto mounts = readKv(mountFiles.front(), profile.escapeMode);
    const auto* root = mounts.root().find("Mounts");
    REQUIRE(root);
    steampp::Steam steam;
    std::vector<fs::path> heads;
    for (const auto& app : root->children()) {
        if (!active(app) || !flag(app, "enabled", true)) continue;
        const auto id = kv1::Node::value("id", std::string(app.key())).asInt();
        REQUIRE(id);
        const auto install = steam.getAppInstallDir(static_cast<std::uint32_t>(*id));
        if (install.empty()) {
            REQUIRE_FALSE(flag(app, "required"));
            continue;
        }
        auto& target = flag(app, "head") ? heads : paths;
        for (const auto& mod : app.children()) {
            if (mod.kind() != kv1::NodeKind::Block || !active(mod)) continue;
            for (const auto& entry : mod.children()) {
                if (!active(entry)) continue;
                auto path = install / mod.key() / normalizePath(entry.decodedValue().value());
                if (ciEquals(entry.key(), "vpk")) {
                    if (!ciEquals(path.extension().string(), ".vpk")) path += ".vpk";
                } else if (!ciEquals(entry.key(), "dir")) continue;
                target.push_back(std::move(path));
            }
        }
    }
    paths.insert(paths.begin(), heads.begin(), heads.end());
    return paths;
}

struct MaterialPaths {
    std::set<std::string> paths;
    std::set<fs::path> directories, archives;
};

MaterialPaths materialPaths(const assets::GameProfile& profile) {
    MaterialPaths result;
    const auto add = [&](std::string_view path) {
        auto key = lookupKey(path);
        if (key.starts_with("materials/") && key.ends_with(".vmt")) result.paths.insert(std::move(key));
    };
    for (const auto& request : mountRequests(profile)) {
        auto paths = expand(request);
        if (paths.empty() && ciEquals(request.extension().string(), ".vpk") &&
            !toLowerAscii(request.stem().string()).ends_with("_dir"))
            paths = expand(request.parent_path() / (request.stem().string() + "_dir.vpk"));
        for (const auto& path : paths) {
            if (fs::is_directory(path)) {
                if (!result.directories.insert(fs::weakly_canonical(path)).second) continue;
                for (const auto& materials : expand(path / "materials")) {
                    for (const auto& entry : fs::recursive_directory_iterator(materials))
                        if (entry.is_regular_file()) add(entry.path().lexically_relative(path).generic_string());
                }
            } else if (ciEquals(path.extension().string(), ".vpk")) {
                auto archive = vpkpp::VPK::open(path.string());
                if (!archive && !toLowerAscii(path.stem().string()).ends_with("_dir")) {
                    for (const auto& fallback : expand(path.parent_path() / (path.stem().string() + "_dir.vpk"))) {
                        archive = vpkpp::VPK::open(fallback.string());
                        if (archive) break;
                    }
                }
                if (!archive) continue; // Vfs also ignores invalid optional archives.
                if (!result.archives.insert(fs::weakly_canonical(archive->getFilepath())).second) continue;
                archive->runForAllEntries([&](const std::string& entry, const vpkpp::Entry&) { add(entry); });
            }
        }
    }
    return result;
}

struct CorpusCounts {
    std::size_t solids = 0, sides = 0, failed = 0, fallback = 0, redundantSolids = 0, redundantSides = 0;
    std::size_t displacementSides = 0, displacementSolids = 0;
    std::size_t nonzero = 0, invalidRotation = 0, uncheckable = 0, positive = 0, negative = 0, defaults = 0;
    std::size_t either = 0, neither = 0;
    std::vector<std::string> examples;
};

std::optional<DVec3> axis(const kv1::Node& side, std::string_view key) {
    std::istringstream input(text(side, key));
    char bracket;
    DVec3 result;
    if (!(input >> bracket) || bracket != '[' || !(input >> result.x >> result.y >> result.z)) return {};
    if (!std::isfinite(glm::length(result)) || glm::length(result) == 0) return {};
    return glm::normalize(result);
}

std::optional<Plane> sidePlane(const kv1::Node& side) {
    std::istringstream input(text(side, "plane"));
    std::array<DVec3, 3> points;
    for (auto& point : points) {
        char open, close;
        if (!(input >> open >> point.x >> point.y >> point.z >> close) || open != '(' || close != ')') return {};
    }
    return geometry::planePointsFromSource(points[0], points[1], points[2]);
}

DVec3 rotate(DVec3 axis, DVec3 normal, double degrees) {
    const double radians = glm::radians(degrees);
    return axis * std::cos(radians) + glm::cross(normal, axis) * std::sin(radians) +
           normal * glm::dot(normal, axis) * (1.0 - std::cos(radians));
}

void countRotation(const kv1::Node& side, const fs::path& file, CorpusCounts& counts) {
    const auto* key = side.find("rotation");
    if (!key) return;
    const auto rotation = key->asDouble();
    if (!rotation || !std::isfinite(*rotation)) {
        ++counts.invalidRotation;
        return;
    }
    if (*rotation == 0) return;
    ++counts.nonzero;
    const auto plane = sidePlane(side);
    const auto u = axis(side, "uaxis"), v = axis(side, "vaxis");
    if (!plane || !u || !v) {
        ++counts.uncheckable;
        return;
    }
    geometry::Face face;
    face.plane = *plane;
    const auto defaults = geometry::generateUv(face, 1, 1);
    const auto agrees = [&](double degrees) {
        const double limit = std::cos(glm::radians(0.5));
        return glm::dot(*u, rotate(defaults.uaxis, plane->normal, degrees)) >= limit &&
               glm::dot(*v, rotate(defaults.vaxis, plane->normal, degrees)) >= limit;
    };
    const bool positive = agrees(*rotation), negative = agrees(-*rotation);
    counts.positive += positive;
    counts.negative += negative;
    counts.defaults += agrees(0);
    counts.either += positive || negative;
    counts.neither += !positive && !negative;
    if (!positive && !negative && counts.examples.size() < 5) {
        std::ostringstream example;
        example << file.generic_string() << " | side " << text(side, "id") << " | rotation " << *rotation
                << " | uaxis " << text(side, "uaxis") << " | vaxis " << text(side, "vaxis");
        counts.examples.push_back(example.str());
    }
}

void countSolid(const kv1::Node& solid, const fs::path& file, CorpusCounts& counts) {
    ++counts.solids;
    bool displacement = false;
    for (const auto* side : solid.findAll("side")) {
        ++counts.sides;
        if (const auto* disp = side->find("dispinfo"); disp && disp->kind() == kv1::NodeKind::Block) {
            ++counts.displacementSides;
            displacement = true;
        }
        countRotation(*side, file, counts);
    }
    counts.displacementSolids += displacement;
    const auto geometry = geometry::solidGeometry(solid);
    if (!geometry) {
        ++counts.failed;
        return;
    }
    counts.fallback += geometry->usedPlaneFallback;
    std::set<std::uint32_t> boundary;
    for (const auto& face : geometry->poly.faces) boundary.insert(face.sideId);
    std::size_t redundant = 0;
    for (const auto& side : geometry->sides) redundant += !boundary.contains(side.sideId);
    counts.redundantSides += redundant;
    counts.redundantSolids += redundant != 0;
}

void walkSolids(const kv1::Node& node, const fs::path& file, CorpusCounts& counts) {
    if (node.kind() == kv1::NodeKind::Block && ciEquals(node.key(), "solid")) {
        countSolid(node, file, counts);
        return;
    }
    for (const auto& child : node.children()) walkSolids(child, file, counts);
}

Result<std::string> materialText(const assets::Vfs& vfs, std::string_view path) {
    auto bytes = vfs.open(path);
    if (!bytes) return std::unexpected(bytes.error());
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

// Material does not expose keywords. Count authored keys, including patch insert/replace.
void keywords(const kv1::Node& node, std::set<std::string>& result, bool& toolTexture) {
    for (const auto& child : node.children()) {
        if (ciEquals(child.key(), "%tooltexture")) toolTexture = true;
        if (ciEquals(child.key(), "%keywords") || ciEquals(child.key(), "$keywords")) {
            const auto value = child.decodedValue();
            if (!value) continue;
            for (const auto word : split(*value, ',')) {
                auto key = toLowerAscii(trim(word));
                if (!key.empty()) result.insert(std::move(key));
            }
        } else if (child.kind() == kv1::NodeKind::Block &&
                   (ciEquals(child.key(), "insert") || ciEquals(child.key(), "replace"))) keywords(child, result, toolTexture);
    }
}

} // namespace

TEST_CASE("[measure] P0 corpus facts for 0.3") {
    const char* enabled = std::getenv("APEX_MEASURE");
    if (!enabled || std::string_view(enabled) != "1") {
        std::cout << "APEX_MEASURE=1 is required; skipping measurements\n";
        return;
    }
    const auto paths = corpusPaths();
    if (paths.empty()) {
        std::cout << "APEX_P2CE_DIR is unset; skipping P0 corpus counts\n";
        return;
    }
    CorpusCounts counts;
    const auto corpusStart = Clock::now();
    for (const auto& path : paths) {
        const auto file = readCorpus(path);
        walkSolids(file.kvDocument().root(), path.lexically_relative(fs::path(std::getenv("APEX_P2CE_DIR"))), counts);
    }
    const double corpusMs = elapsed(corpusStart);

    assets::GameProfile profile;
    profile.name = "Portal 2: Community Edition";
    profile.gameDirectory = fs::path(std::getenv("APEX_P2CE_DIR")) / "p2ce";
    const auto indexStart = Clock::now();
    const auto materials = materialPaths(profile);
    const double indexMs = elapsed(indexStart);
    JobPool jobs(1);
    const auto vfsStart = Clock::now();
    const auto vfs = assets::Vfs::load(profile, jobs);
    REQUIRE(vfs);
    const double vfsMs = elapsed(vfsStart);
    std::size_t parsed = 0, failed = 0, keywordFailures = 0, withKeywords = 0, toolTextures = 0, authoredTools = 0;
    std::map<std::string, std::size_t> frequencies, shaders;
    std::vector<std::string> errors;
    const auto parseStart = Clock::now();
    for (const auto& path : materials.paths) {
        const auto material = assets::loadMaterial(*vfs, path);
        if (material) {
            ++parsed;
            ++shaders[toLowerAscii(material->shader)];
            toolTextures += !material->toolTexture.empty();
        } else {
            ++failed;
            if (errors.size() < 5) errors.push_back(path + " | " + material.error().message);
        }
        auto bytes = materialText(*vfs, path);
        REQUIRE(bytes);
        const auto raw = kv1::parse(std::move(*bytes), profile.escapeMode);
        if (!raw) {
            ++keywordFailures;
            continue;
        }
        const auto expanded = raw->resolve([&](std::string_view include) { return materialText(*vfs, include); });
        if (!expanded) {
            ++keywordFailures;
            continue;
        }
        std::set<std::string> words;
        bool toolTexture = false;
        for (const auto& root : expanded->root().children()) keywords(root, words, toolTexture);
        authoredTools += toolTexture;
        withKeywords += !words.empty();
        for (const auto& word : words) ++frequencies[word];
    }
    const double parseMs = elapsed(parseStart);
    std::vector<std::pair<std::string, std::size_t>> ranked(frequencies.begin(), frequencies.end());
    std::ranges::sort(ranked, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::cout << std::fixed << std::setprecision(3)
              << "\n=== P0 corpus facts (steady_clock, milliseconds) ===\n"
              << "VMFs | " << paths.size() << "\nsolids | " << counts.solids << "\nsource sides | " << counts.sides
              << "\ngeometry build failures | " << counts.failed << "\ngeometry plane fallbacks | " << counts.fallback
              << "\nredundant-source solids (built only) | " << counts.redundantSolids
              << "\nredundant source sides (IDs absent from boundary faces) | " << counts.redundantSides
              << "\ndispinfo sides | " << counts.displacementSides << "\nsolids with dispinfo | " << counts.displacementSolids
              << "\nnonzero rotation sides | " << counts.nonzero << "\ninvalid rotation values | " << counts.invalidRotation
              << "\nnonzero rotation uncheckable (plane/axes) | " << counts.uncheckable
              << "\nrotation agreement +rotation (both axes, 0.5 deg) | " << counts.positive
              << "\nrotation agreement -rotation (both axes, 0.5 deg) | " << counts.negative
              << "\nrotation agreement either sign | " << counts.either
              << "\nrotation agreement neither sign | " << counts.neither
              << "\naxes equal default among nonzero rotation sides | " << counts.defaults
              << "\ncorpus parse/count ms | " << corpusMs << "\nrotation disagreement examples (relative to install):\n";
    for (const auto& example : counts.examples) std::cout << example << '\n';

    std::cout << "material loose mounts (unique physical dirs) | " << materials.directories.size()
              << "\nmaterial VPK mounts (unique archives) | " << materials.archives.size()
              << "\ndistinct canonical materials/*.vmt paths (recursive) | " << materials.paths.size()
              << "\nmaterial enumeration/index ms (mount discovery included) | " << indexMs
              << "\nVfs load ms (separate, all loose assets) | " << vfsMs
              << "\nVMT parse/extract ms (shader + authored keywords, includes I/O) | " << parseMs
              << "\nVMT loadMaterial successes | " << parsed << "\nVMT loadMaterial failures | " << failed
              << "\nVMT keyword parse/resolve failures | " << keywordFailures
              << "\ndistinct resolved shaders | " << shaders.size()
              << "\nVMTs with authored %keywords/$keywords | " << withKeywords
              << "\ndistinct keywords (comma-split, trimmed, lowercase) | " << frequencies.size()
              << "\nVMTs with authored %tooltexture key | " << authoredTools
              << "\nVMTs with nonempty resolved %tooltexture | " << toolTextures
              << "\nkeyword top 30 (frequency = distinct VMT paths, authored keys, no patch inheritance):\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(30, ranked.size()); ++i)
        std::cout << ranked[i].first << " | " << ranked[i].second << '\n';
    std::cout << "VMT load failure examples:\n";
    for (const auto& error : errors) std::cout << error << '\n';
    std::cout << "=== end P0 corpus facts ===\n";
}
