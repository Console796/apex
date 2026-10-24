#include "assets/vmt.hpp"
#include "document/vmf.hpp"
#include "core/paths.hpp"

#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace apex;
using namespace apex::assets;
namespace {
std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>(in), {}};
}
template <typename T> void collect(const T& node, std::set<std::string>& materials) {
    if constexpr (requires { node.sides; }) {
        for (const auto& side : node.sides) {
            auto value = side.material();
            REQUIRE(value);
            auto text = value->text();
            REQUIRE(text);
            materials.insert(lookupKey(*text));
        }
    }
    if constexpr (requires { node.solids; }) {
        for (const auto& child : node.solids) collect(child, materials);
    }
    if constexpr (requires { node.groups; }) {
        for (const auto& child : node.groups) collect(child, materials);
    }
    if constexpr (requires { node.hidden; }) {
        for (const auto& child : node.hidden) collect(child, materials);
    }
    if constexpr (requires { node.entities; }) {
        for (const auto& child : node.entities) collect(child, materials);
    }
}
}
TEST_CASE("Materials referenced by all corpus VMF sides match reviewed missing list") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    auto root = std::filesystem::path(install);
    std::set<std::string> materials;
    std::size_t maps = 0;
    for (const auto& file : std::filesystem::recursive_directory_iterator(root / "sdk_content/maps")) {
        if (!file.is_regular_file() || file.path().extension() != ".vmf") {
            continue;
        }
        auto vmf = vmf::parse(readFile(file.path()));
        REQUIRE_MESSAGE(vmf, file.path().string());
        if (vmf->world) {
            collect(*vmf->world, materials);
        }
        collect(*vmf, materials);
        ++maps;
    }
    // test1.vmf and hammertest.vmf ship with the P2CE SDK content, not with the instances clone;
    // an install without them is still a valid corpus (they are two of the 512 maps).
    std::size_t expectedMaps = 510;
    for (const char* name : {"test1.vmf", "hammertest.vmf"})
        if (std::filesystem::exists(root / "sdk_content/maps" / name)) ++expectedMaps;
    REQUIRE(maps == expectedMaps);
    REQUIRE_FALSE(materials.empty());
    GameProfile profile;
    profile.gameDirectory = root / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    std::ostringstream actual;
    std::size_t missing = 0;
    for (const auto& name : materials) {
        auto material = loadMaterial(*vfs, name);
        std::string reason;
        if (!material) {
            reason = "VMT: " + material.error().what();
        } else if (material->shader.empty()) {
            reason = "empty shader";
        } else if (material->baseTexture.empty()) {
            reason = "no $basetexture (shader " + material->shader + ")";
        } else if (!vfs->exists(texturePath(material->baseTexture))) {
            reason = "VTF missing: " + texturePath(material->baseTexture);
        }
        if (!reason.empty()) {
            actual << name << " | " << reason << '\n';
            ++missing;
        }
    }
    MESSAGE(maps, " VMFs, ", materials.size(), " unique materials, ", missing, " missing/incomplete");
    // Optional local report for reviewing intentional baseline updates; never overwrites it.
    if (const auto* report = std::getenv("APEX_MISSING_REPORT")) {
        std::ofstream out(report);
        REQUIRE(out);
        out << actual.str();
    }
    auto expected = readFile(std::filesystem::path(APEX_FIXTURE_DIR) / "assets/expected_missing_materials.txt");
    // The baseline starts with an Apex-authored analysis comment.
    std::istringstream lines(expected);
    std::string line, baseline;
    while (std::getline(lines, line)) {
        if (line.empty() || line.front() == '#') continue;
        // Without the two loose maps, baseline materials only they reference are not in the corpus.
        if (expectedMaps < 512 && !materials.contains(line.substr(0, line.find(" | ")))) continue;
        baseline += line + '\n';
    }
    CHECK(actual.str() == baseline);
}
