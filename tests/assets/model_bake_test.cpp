#include "assets/model_bake.hpp"
#include "assets/model_pose.hpp"
#include <cstring>
#include <limits>
#include <glm/gtc/constants.hpp>

#include <doctest/doctest.h>
#include <mdlpp/mdlpp.h>

#include <array>
#include <set>
#include <string>
#include <cstdint>
#include <vector>

using namespace apex;
using namespace apex::assets;

namespace {
// Owns strip spans through StripGroup's heap vectors. Never copy after wiring the spans.
void triangleModel(mdlpp::StudioModel& model, std::size_t choices, std::int32_t secondOffset) {
    model.mdl.bodyParts.resize(1);
    auto& body = model.mdl.bodyParts[0];
    body.name = "body";
    body.base = 1;
    body.models.resize(choices);
    model.vtx.numLODs = 1;
    model.vtx.bodyParts.resize(1);
    model.vtx.bodyParts[0].models.resize(choices);
    model.mdl.materials.resize(1);
    model.mdl.materials[0].name = "test/model";
    model.mdl.skins = {{0}};
    for (std::size_t choice = 0; choice < choices; ++choice) {
        auto& mdl = body.models[choice];
        mdl.name = std::to_string(choice);
        mdl.verticesCount = choice == 0 && choices > 1 ? secondOffset : 3;
        // Model vertex offsets are byte offsets (48-byte mstudiovertex_t), mesh offsets are counts.
        mdl.verticesOffset = choice == 0 ? 0 : secondOffset * 48;
        mdl.meshes.resize(1);
        mdl.meshes[0].verticesCount = mdl.verticesCount;
        auto& vtx = model.vtx.bodyParts[0].models[choice];
        vtx.modelLODs.resize(1);
        vtx.modelLODs[0].meshes.resize(1);
        auto& mesh = vtx.modelLODs[0].meshes[0];
        mesh.stripGroups.resize(1);
        auto& group = mesh.stripGroups[0];
        group.vertices.resize(3);
        for (std::uint16_t i = 0; i < 3; ++i) group.vertices[i].meshVertexID = i;
        group.indices = std::vector<std::uint16_t>{0, 1, 2};
        group.strips.resize(1);
        group.strips[0].flags = mdlpp::VTX::Strip::FLAG_IS_TRILIST;
        group.strips[0].indices = group.indices;
        group.strips[0].vertices = group.vertices;
    }
    const auto count = choices == 1 ? 3 : secondOffset + 3;
    model.vvd.numLODs = 1;
    model.vvd.numVerticesInLOD[0] = count;
    model.vvd.vertices.resize(static_cast<std::size_t>(count));
}
}

TEST_CASE("model bake retains absolute uint32 vertex IDs beyond 65535") {
    mdlpp::StudioModel source{};
    constexpr std::uint32_t offset = 65536 + 7;
    triangleModel(source, 2, static_cast<std::int32_t>(offset));
    const auto baked = bakeModel(source.mdl, source.vtx, source.vvd);
    INFO((baked ? "baked" : baked.error().what()));
    REQUIRE(baked.has_value());
    const std::vector<std::uint32_t> expected{0, 2, 1, offset, offset + 2, offset + 1};
    CHECK(baked->indices == expected);
    for (const auto index : baked->indices) CHECK(index < baked->vertices.size());
    // The convenience overload must also work on structs populated without open().
    const auto convenience = bakeModel(source);
    REQUIRE(convenience);
    CHECK(convenience->indices == expected);
}

TEST_CASE("model bake documentation: uint16 pipeline wraps absolute vertex IDs") {
    constexpr std::uint32_t absolute = 65536 + 7;
    const auto wrapped = static_cast<std::uint16_t>(absolute);
    CHECK(wrapped == 7);
    CHECK(static_cast<std::uint32_t>(wrapped) != absolute);
}

TEST_CASE("model bake short strips and incomplete trilists never read past indices") {
    // Separate heap vectors for every strip: no adjacent strip can hide an ASan over-read.
    for (const std::size_t shortSize : {0u, 1u, 2u}) {
        CAPTURE(shortSize);
        mdlpp::StudioModel source{};
        triangleModel(source, 1, 0);
        auto& mesh = source.vtx.bodyParts[0].models[0].modelLODs[0].meshes[0];
        mesh.stripGroups.resize(3);
        for (std::size_t i = 0; i < mesh.stripGroups.size(); ++i) {
            auto& group = mesh.stripGroups[i];
            group.vertices = std::vector<mdlpp::VTX::Vertex>(3);
            for (std::uint16_t v = 0; v < 3; ++v) group.vertices[v].meshVertexID = v;
            const auto size = i == 0 ? shortSize : (i == 1 ? 2u : 3u);
            group.indices = std::vector<std::uint16_t>(size);
            for (std::size_t j = 0; j < size; ++j) group.indices[j] = static_cast<std::uint16_t>(j);
            group.strips.resize(1);
            group.strips[0].indices = group.indices;
            group.strips[0].vertices = group.vertices;
            group.strips[0].flags = i == 1 ? mdlpp::VTX::Strip::FLAG_IS_TRILIST
                                         : mdlpp::VTX::Strip::FLAG_IS_TRISTRIP;
        }
        const auto baked = bakeModel(source.mdl, source.vtx, source.vvd);
        INFO((baked ? "baked" : baked.error().what()));
        REQUIRE(baked.has_value());
        // Only the normal strip contributes, with Source's winding flip.
        CHECK(baked->indices == std::vector<std::uint32_t>{1, 2, 0});
    }
}

TEST_CASE("model skin selection remaps material references and falls back to skin zero") {
    ModelData model;
    model.materialNames = {"red", "blue", "green"};
    model.materialDirectories = {"models/a", "models/b"};
    model.skins = {{2, 0}, {1, 2}};
    model.bodyParts = {{"body", 1, {{"default", {}}}}};
    model.submeshes = {{0, 3, 0, 0, 0, 0}, {3, 6, 0, 0, 1, 1}, {9, 3, 0, 0, 2, 7}};
    const auto familyOne = modelDrawRanges(model, 0, 1);
    REQUIRE(familyOne.size() == 3);
    CHECK(familyOne[0].material == 1);
    CHECK(familyOne[1].material == 2);
    CHECK(familyOne[2].material == 7); // Out-of-range skin reference stays raw.
    const auto fallback = modelDrawRanges(model, 0, 99);
    REQUIRE(fallback.size() == 3);
    CHECK(fallback[0].material == 2);
    CHECK(fallback[1].material == 0);
}

TEST_CASE("model bake rejects corrupt mdl vtx and vvd bytes and out-of-range vertex IDs") {
    for (const auto malformedSize : {0u, 1u, 7u, 31u}) {
        CAPTURE(malformedSize);
        const auto garbage = std::vector<std::byte>(malformedSize, std::byte{0x7f});
        mdlpp::StudioModel parsed{};
        bool opened = false;
        try { opened = parsed.open(garbage, garbage, garbage); } catch (const std::exception&) { }
        CHECK_FALSE(opened);
        CHECK_FALSE(bakeModel(parsed));
        for (int component = 0; component < 3; ++component) {
            auto mdl = garbage, vtx = garbage, vvd = garbage;
            (component == 0 ? mdl : component == 1 ? vtx : vvd).resize(malformedSize / 2);
            mdlpp::StudioModel candidate{};
            bool candidateOpened = false;
            try { candidateOpened = candidate.open(mdl, vtx, vvd); } catch (const std::exception&) { }
            CHECK_FALSE(candidateOpened);
            CHECK_FALSE(bakeModel(candidate));
        }
    }
    mdlpp::StudioModel source{};
    triangleModel(source, 1, 0);
    source.vtx.bodyParts[0].models[0].modelLODs[0].meshes[0].stripGroups[0].vertices[0].meshVertexID = 9;
    const auto baked = bakeModel(source.mdl, source.vtx, source.vvd);
    REQUIRE_FALSE(baked);
    CHECK(baked.error().message.starts_with("vertex id out of range"));
}

TEST_CASE("model bake rejects strip spans that point outside their strip group") {
    for (const auto kind : {0, 1, 2}) {
        CAPTURE(kind);
        mdlpp::StudioModel source{};
        triangleModel(source, 1, 0);
        auto& group = source.vtx.bodyParts[0].models[0].modelLODs[0].meshes[0].stripGroups[0];
        static std::vector<std::uint16_t> elsewhere{0, 1, 2};
        if (kind == 0) group.strips[0].indices = std::span<std::uint16_t>(elsewhere); // another allocation
        if (kind == 1) group.strips[0].indices = std::span<std::uint16_t>(group.indices.data() + 1, 3); // runs past the end
        if (kind == 2) group.strips[0].indices = std::span<std::uint16_t>(group.indices.data(), std::size_t{1} << 40);
        const auto baked = bakeModel(source.mdl, source.vtx, source.vvd);
        REQUIRE_FALSE(baked);
        CHECK(baked.error().message.starts_with("strip range out of bounds"));
    }
}

TEST_CASE("model material resolution walks cdmaterials in order and normalises names") {
    ModelData model;
    model.materialNames = {"Metal_Wall", "shared\\Decal", "nowhere"};
    model.materialDirectories = {"models\\Props\\", "models/props_extra", "/materials_root"};
    const std::set<std::string> vmts{"materials/models/props_extra/metal_wall.vmt",
                                     "materials/models/props/metal_wall.vmt",
                                     "materials/materials_root/shared/decal.vmt"};
    std::vector<std::string> asked;
    const auto exists = [&](const std::string& path) {
        asked.push_back(path);
        return vmts.contains(path);
    };
    // The first directory in the model's order wins even when a later one also holds the name.
    CHECK(resolveModelMaterial(model, 0, exists) == "models/props/metal_wall");
    CHECK(asked == std::vector<std::string>{"materials/models/props/metal_wall.vmt"});
    // Backslashes, case, a missing trailing slash and a leading slash on the directory all normalise.
    CHECK(resolveModelMaterial(model, 1, exists) == "materials_root/shared/decal");
    // Nothing holds it, or the index is out of range: empty, so the caller draws the fallback material.
    CHECK(resolveModelMaterial(model, 2, exists).empty());
    CHECK(resolveModelMaterial(model, 3, exists).empty());
    CHECK(resolveModelMaterial(model, -1, exists).empty());
    model.materialDirectories.clear();
    CHECK(resolveModelMaterial(model, 0, exists).empty());
}

namespace {
template <typename T> void animationField(std::vector<std::byte>& bytes, std::size_t at, T value) {
    REQUIRE(at + sizeof(value) <= bytes.size());
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}
std::vector<std::byte> animatedTriangle(mdlpp::StudioModel& source) {
    triangleModel(source, 1, 0);
    source.mdl.bones.resize(1);
    auto& bone = source.mdl.bones[0];
    bone.parent = -1;
    bone.rotationQuat[3] = 1;
    bone.positionScale[0] = 1;
    for (int r = 0; r < 3; ++r) bone.poseToBose[r][r] = 1;
    source.mdl.sequences.resize(1);
    source.mdl.sequences[0].label = "slide";
    source.mdl.sequences[0].animIndices = {0};
    for (auto& vertex : source.vvd.vertices) {
        vertex.position[0] = 1;
        vertex.normal[1] = 1;
        vertex.boneWeight.bones = {0};
        vertex.boneWeight.weight = {1};
    }
    std::vector<std::byte> bytes(512);
    animationField<std::int32_t>(bytes, 180, 1);
    animationField<std::int32_t>(bytes, 184, 192);
    animationField<float>(bytes, 200, 30);
    animationField<std::int32_t>(bytes, 208, 7);
    animationField<std::int32_t>(bytes, 248, 100);
    bytes[293] = std::byte{0x04}; // Animated position, one bone, no following entry.
    animationField<std::int16_t>(bytes, 296, 6); // X axis value stream.
    animationField<std::uint16_t>(bytes, 302, 0x0503); // Three values, five total frames.
    animationField<std::int16_t>(bytes, 304, 0);
    animationField<std::int16_t>(bytes, 306, 10);
    animationField<std::int16_t>(bytes, 308, 20);
    animationField<std::uint16_t>(bytes, 310, 0x0202);
    animationField<std::int16_t>(bytes, 312, 30);
    animationField<std::int16_t>(bytes, 314, 40);
    return bytes;
}
}

TEST_CASE("model animation evaluates RLE hold spans and fractional frames from retained vertex weights") {
    mdlpp::StudioModel source{};
    auto bytes = animatedTriangle(source);
    const auto baked = bakeModel(source, bytes);
    REQUIRE(baked);
    REQUIRE(baked->animation);
    REQUIRE(baked->sequences.size() == 1);
    CHECK(baked->sequences[0].name == "slide");
    CHECK(baked->sequences[0].supported);
    CHECK(baked->sequences[0].frames == 7);
    CHECK(baked->sequences[0].fps == doctest::Approx(30));
    for (const auto [frame, translation] : std::array<std::pair<double, double>, 8>{
             {{0, 0}, {1, 10}, {2, 20}, {3, 20}, {4, 20}, {4.5, 25}, {5.5, 35}, {100, 40}}}) {
        const auto posed = animateModel(*baked, 0, frame);
        REQUIRE(posed.size() == 3);
        CHECK(posed[0].position.x == doctest::Approx(1 + translation));
        CHECK(posed[0].normal.y == doctest::Approx(1));
    }
    CHECK(baked->vertices[0].position.x == doctest::Approx(1)); // Cached initial pose is immutable.
    CHECK(animateModel(*baked, -1, 0).empty());
    CHECK(animateModel(*baked, 99, 0).empty());
    CHECK(animateModel(*baked, 0, std::numeric_limits<double>::quiet_NaN()).empty());
    CHECK(modelAnimationByteSize(*baked) > bytes.size());
}

TEST_CASE("model animation interpolates local quaternion rotations without shrinking the mesh") {
    mdlpp::StudioModel source{};
    auto bytes = animatedTriangle(source);
    source.mdl.bones[0].rotationScale[2] = static_cast<float>(glm::half_pi<double>());
    animationField<std::int32_t>(bytes, 208, 2);
    bytes[293] = std::byte{0x08}; // Animated Euler rotation around Z.
    animationField<std::int16_t>(bytes, 296, 0);
    animationField<std::int16_t>(bytes, 300, 6);
    animationField<std::uint16_t>(bytes, 302, 0x0202);
    animationField<std::int16_t>(bytes, 304, 0);
    animationField<std::int16_t>(bytes, 306, 1);
    const auto baked = bakeModel(source, bytes);
    REQUIRE(baked);
    REQUIRE(baked->sequences[0].supported);
    const auto posed = animateModel(*baked, 0, 0.5);
    REQUIRE(posed.size() == 3);
    CHECK(posed[0].position.x == doctest::Approx(std::sqrt(0.5)).epsilon(1e-5));
    CHECK(posed[0].position.y == doctest::Approx(std::sqrt(0.5)).epsilon(1e-5));
    CHECK(glm::length(posed[0].position) == doctest::Approx(1));
    CHECK(glm::length(posed[0].normal) == doctest::Approx(1));
}

TEST_CASE("model animation reports unsupported sequences and rejects malformed tracks") {
    mdlpp::StudioModel source{};
    auto bytes = animatedTriangle(source);
    SUBCASE("external animation block") { animationField<std::int32_t>(bytes, 244, 1); }
    SUBCASE("delta animation") { animationField<std::int32_t>(bytes, 204, 4); }
    SUBCASE("pose blending") { source.mdl.sequences[0].animIndices = {0, 0}; }
    SUBCASE("truncated RLE stream") { bytes.resize(306); }
    SUBCASE("invalid RLE span") { animationField<std::uint16_t>(bytes, 302, 0x0203); }
    SUBCASE("invalid bone parent") { source.mdl.bones[0].parent = 0; }
    const auto baked = bakeModel(source, bytes);
    REQUIRE(baked);
    REQUIRE(baked->sequences.size() == 1);
    CHECK_FALSE(baked->sequences[0].supported);
    CHECK_FALSE(baked->sequences[0].reason.empty());
    CHECK_FALSE(baked->animation);
    CHECK(animateModel(*baked, 0, 0).empty());
}
