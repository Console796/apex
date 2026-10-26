#include "assets/model_bake.hpp"
#include "assets/vfs.hpp"

#include <doctest/doctest.h>
#include <mdlpp/mdlpp.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <numeric>

using namespace apex;
using namespace apex::assets;

TEST_CASE("model bake corpus bodygroups select separate choice ranges") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    // Scanned all 510 sdk_content/maps VMFs with srctools Keyvalues, recursively inspecting
    // connections and resolving SetBodyGroup targets to entities' model keys. There are no
    // such inputs: the 294 visible occurrences (12 more hidden) are entity keys. The 41
    // distinct models on visible entities with that key have no multi-choice body parts.
    // Use portal2/portal2/portal2_dir.vpk's models/effects/cappoint_hologram.mdl instead:
    // its Body part has four choices, three nonempty. No shipped bytes are test fixtures.
    const std::string stem = "models/effects/cappoint_hologram";
    GameProfile profile;
    profile.gameDirectory = std::filesystem::path(install) / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    const auto mdl = vfs->open(stem + ".mdl");
    const auto vtx = vfs->open(stem + ".dx90.vtx");
    const auto vvd = vfs->open(stem + ".vvd");
    REQUIRE(mdl);
    REQUIRE(vtx);
    REQUIRE(vvd);
    mdlpp::StudioModel source{};
    REQUIRE(source.open(*mdl, *vtx, *vvd));
    const auto part = std::ranges::find_if(source.mdl.bodyParts, [](const auto& body) {
        return std::ranges::count_if(body.models, [](const auto& choice) {
            return choice.verticesCount > 0;
        }) > 1;
    });
    REQUIRE(part != source.mdl.bodyParts.end());
    const auto bodyPart = static_cast<std::uint32_t>(part - source.mdl.bodyParts.begin());
    const auto baked = bakeModel(source);
    INFO((baked ? "baked" : baked.error().what()));
    REQUIRE(baked.has_value());
    REQUIRE(bodyPart < baked->bodyParts.size());
    CHECK(baked->bodyParts[bodyPart].choices.size() == part->models.size());
    std::vector<std::uint32_t> choices;
    for (std::size_t i = 0; i < part->models.size(); ++i) {
        if (part->models[i].verticesCount > 0) choices.push_back(static_cast<std::uint32_t>(i));
    }
    REQUIRE(choices.size() > 1);
    const auto allIndices = std::accumulate(baked->submeshes.begin(), baked->submeshes.end(),
                                           std::size_t{0}, [](auto sum, const auto& mesh) {
                                               return sum + mesh.indexCount;
                                           });
    for (const auto choice : {choices[0], choices[1]}) {
        const auto body = choice * static_cast<std::uint32_t>(part->base);
        const auto selected = modelBodyChoices(*baked, body);
        REQUIRE(selected.size() == baked->bodyParts.size());
        CHECK(selected[bodyPart] == choice);
        const auto ranges = modelDrawRanges(*baked, body, 0);
        REQUIRE_FALSE(ranges.empty());
        std::size_t selectedIndices = 0;
        for (const auto& range : ranges) {
            selectedIndices += range.indexCount;
            const auto submesh = std::ranges::find_if(baked->submeshes, [&](const auto& mesh) {
                return mesh.firstIndex == range.firstIndex && mesh.indexCount == range.indexCount;
            });
            REQUIRE(submesh != baked->submeshes.end());
            CHECK(submesh->choice == selected[submesh->bodyPart]);
        }
        CHECK(selectedIndices > 0);
        CHECK(selectedIndices < allIndices);
        for (const auto& mesh : baked->submeshes) {
            if (mesh.bodyPart == bodyPart && mesh.choice == choice && mesh.indexCount > 0) {
                CHECK(std::ranges::any_of(ranges, [&](const auto& range) {
                    return range.firstIndex == mesh.firstIndex && range.indexCount == mesh.indexCount;
                }));
                for (const auto& other : baked->submeshes) {
                    if (other.bodyPart == bodyPart && other.choice != choice && other.indexCount > 0)
                        CHECK((mesh.firstIndex + mesh.indexCount <= other.firstIndex ||
                               other.firstIndex + other.indexCount <= mesh.firstIndex));
                }
            }
        }
    }
}
