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
        mdl.verticesOffset = choice == 0 ? 0 : secondOffset;
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
