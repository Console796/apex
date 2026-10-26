#include "assets/model_bake.hpp"

#include <doctest/doctest.h>
#include <mdlpp/mdlpp.h>

#include <array>
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
