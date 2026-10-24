#include "document/vmf.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::filesystem::path fixtures() {
    return std::filesystem::path(APEX_FIXTURE_DIR) / "vmf";
}

apex::vmf::VmfFile parsed(std::string_view source) {
    auto result = apex::vmf::parse(source);
    REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().what()));
    return std::move(*result);
}

} // namespace

TEST_CASE("VMF fixtures and probe CRLF input write byte-identically") {
    for (const auto& entry : std::filesystem::directory_iterator(fixtures())) {
        CAPTURE(entry.path().string());
        const auto source = read(entry.path());
        auto file = parsed(source);
        CHECK(apex::vmf::write(file) == source);
        const auto expected = entry.path().filename() == "minimal_crlf.vmf" ? "\r\n" : "\n";
        CHECK(file.lineEnding() == expected);
    }

    const auto path = std::filesystem::path(APEX_FIXTURE_DIR) / "probe" / "e_control.vmf";
    const auto source = read(path);
    auto file = parsed(source);
    CHECK(file.lineEnding() == "\r\n");
    CHECK(apex::vmf::write(file) == source);
}

TEST_CASE("VMF schema exposes typed known blocks and retains raw values") {
    auto file = parsed(read(fixtures() / "everything_lf.vmf"));
    REQUIRE(file.versionInfo);
    REQUIRE(file.versionInfo->editorExtension());
    CHECK(*file.versionInfo->editorExtension()->text() == "strata");
    REQUIRE(file.visgroups);
    REQUIRE(file.visgroups->children.size() == 1);
    CHECK(file.visgroups->children[0].children.size() == 1);
    REQUIRE(file.viewSettings);
    REQUIRE(file.viewSettings->views);
    CHECK(file.viewSettings->views->children.size() == 1);
    CHECK(*file.viewSettings->instanceVisibility()->asInt() == 2);
    REQUIRE(file.world);
    REQUIRE(file.world->solids.size() == 1);
    auto& side = file.world->solids[0].sides[0];
    CHECK(side.rotation()->raw() == "15.9999");
    CHECK(*side.rotation()->asDouble() == doctest::Approx(15.9999));
    const auto plane = side.plane()->asPlane();
    REQUIRE(plane);
    CHECK(plane->points[1] == apex::vmf::Vec3{0.0, 64.0, 0.0});
    const auto uv = side.uaxis()->asUvAxis();
    REQUIRE(uv);
    CHECK(uv->axis == apex::vmf::Vec3{1.0, 0.0, 0.0});
    CHECK(uv->shift == 8.0);
    CHECK(uv->scale == 0.25);
    REQUIRE(side.pointData);
    REQUIRE(side.pointData->points());
    CHECK(side.pointData->points()->size() == 3);
    REQUIRE(side.verticesPlus);
    REQUIRE(side.verticesPlus->points());
    CHECK(side.verticesPlus->points()->size() == 3);
    REQUIRE(side.dispInfo);
    CHECK(side.unknownBlocks.size() == 1);
    CHECK(file.entities.size() == 1);
    CHECK(file.world->hidden[0].entities.size() == 1);
    CHECK(file.entities[0].findAll("targetname").size() == 2);
    REQUIRE(file.cameras);
    CHECK(file.cameras->cameras.size() == 1);
    REQUIRE(file.cordons);
    CHECK(file.cordons->cordons.size() == 1);
    CHECK(file.unknownBlocks.size() == 1);
    CHECK(file.problems.empty());
}

TEST_CASE("VMF one-solid edit changes no bytes outside that solid block") {
    const auto source = read(fixtures() / "everything_lf.vmf");
    auto file = parsed(source);
    REQUIRE(file.world);
    auto& solid = file.world->solids[0];
    const auto span = solid.node->sourceSpan();
    REQUIRE(solid.sides[0].material()->set("DEV/NEW_MATERIAL"));
    const auto output = apex::vmf::write(file);
    CHECK(output.substr(0, span.offset) == source.substr(0, span.offset));
    const auto oldSuffix = span.offset + span.length;
    const auto delta = output.size() - source.size();
    const auto newSuffix = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(oldSuffix) +
                                                    static_cast<std::ptrdiff_t>(delta));
    CHECK(output.substr(newSuffix) == source.substr(oldSuffix));
    CHECK(output.find("DEV/NEW_MATERIAL") != std::string::npos);
    CHECK(output.find("apex_unknown_top_level") == source.find("apex_unknown_top_level") + delta);
}

TEST_CASE("VMF tracks object and side IDs independently and reports duplicates") {
    const std::string source =
        "world { id 1 solid { id 2 side { id 1 } side { id 1 } } }\n"
        "entity { id 2 classname test }\n";
    auto file = parsed(source);
    CHECK(file.objectIds.count(1) == 0);
    CHECK(file.objectIds.count(2) == 2);
    CHECK(file.sideIds.count(1) == 2);
    CHECK(file.objectIds.maximum() == 2);
    CHECK(file.sideIds.maximum() == 1);
    REQUIRE(file.problems.size() == 2);
    CHECK(file.problems[0].code == apex::vmf::ProblemCode::DuplicateSideId);
    CHECK(file.problems[1].code == apex::vmf::ProblemCode::DuplicateObjectId);
}

TEST_CASE("VMF malformed input returns a contextual error") {
    for (const auto source : {"world {", "entity { id \"unterminated", "side }"}) {
        const auto result = apex::vmf::parse(source);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == apex::ErrorCode::ParseError);
        CHECK(result.error().what().find("VMF") != std::string::npos);
    }
}

TEST_CASE("VMF optional loose P2CE corpus round trips") {
    const char* root = std::getenv("APEX_P2CE_DIR");
    if (!root || !std::filesystem::is_directory(root)) {
        MESSAGE("APEX_P2CE_DIR is unset or invalid; skipping optional VMF corpus");
        return;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root,
                                                       std::filesystem::directory_options::
                                                           skip_permission_denied)) {
        if (entry.is_regular_file() && entry.path().extension() == ".vmf") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    REQUIRE_MESSAGE(!files.empty(), "no VMFs found below APEX_P2CE_DIR");

    std::size_t bytes = 0;
    std::size_t largestBytes = 0;
    std::filesystem::path largest;
    std::chrono::nanoseconds largestTime{};
    for (const auto& path : files) {
        CAPTURE(path.string());
        const auto source = read(path);
        const auto start = std::chrono::steady_clock::now();
        auto file = apex::vmf::parse(source);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE_MESSAGE(file.has_value(), (file ? "" : file.error().what()));
        CHECK(apex::vmf::write(*file) == source);
        bytes += source.size();
        if (source.size() > largestBytes) {
            largestBytes = source.size();
            largest = path;
            largestTime = elapsed;
        }
    }
    // hammertest.vmf is a loose map outside the instances clone; an install without it is still a
    // valid corpus. test1.vmf was never corpus: it was the owner's own test map.
    std::size_t expectedFiles = 510;
    for (const char* name : {"hammertest.vmf"})
        if (std::filesystem::exists(std::filesystem::path(root) / "sdk_content/maps" / name)) ++expectedFiles;
    CHECK(files.size() == expectedFiles);
    const auto milliseconds =
        std::chrono::duration<double, std::milli>(largestTime).count();
    MESSAGE("VMF corpus: ", files.size(), " files, ", bytes, " bytes; largest ",
            largest.filename().string(), " (", largestBytes, " bytes) parsed in ", milliseconds,
            " ms (release target < 50 ms)");
}
