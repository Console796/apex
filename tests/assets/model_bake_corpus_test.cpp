#include "assets/model_bake.hpp"
#include "assets/vfs.hpp"
#include "geometry/bvh.hpp"

#include <doctest/doctest.h>
#include <mdlpp/mdlpp.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <numeric>
#include <optional>
#include <thread>

using namespace apex;
using namespace apex::assets;

TEST_CASE("model bake corpus bodygroups select separate choice ranges") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    // Scanned all 510 sdk_content/maps VMFs for SetBodyGroup connections and resolved output
    // targets to their model entities. That scan did not find a multi-choice target model, so
    // select the first mounted corpus model with multiple nonempty, bakeable choices instead.
    GameProfile profile;
    profile.gameDirectory = std::filesystem::path(install) / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    std::optional<ModelData> baked;
    std::string modelName;
    std::size_t bodyPart = 0, partChoiceCount = 0;
    std::uint32_t partBase = 1;
    std::vector<std::uint32_t> choices;
    std::string candidateErrors;
    std::size_t multichoiceModels = 0, multichoiceBakeSuccesses = 0;
    const auto candidates = vfs->walk("models", "*.mdl");
    for (const auto& mdlPath : candidates) {
        const auto stem = mdlPath.substr(0, mdlPath.size() - 4);
        const auto mdl = vfs->open(mdlPath);
        const auto vtx = vfs->open(stem + ".dx90.vtx");
        const auto vvd = vfs->open(stem + ".vvd");
        if (!mdl || !vtx || !vvd) continue;
        mdlpp::StudioModel source{};
        try {
            if (!source.open(*mdl, *vtx, *vvd)) continue;
        } catch (const std::exception&) {
            continue;
        }
        const auto part = std::ranges::find_if(source.mdl.bodyParts, [](const auto& body) {
            return std::ranges::count_if(body.models, [](const auto& choice) {
                return choice.verticesCount > 0;
            }) > 1;
        });
        if (part == source.mdl.bodyParts.end()) continue;
        ++multichoiceModels;
        auto result = bakeModel(source);
        if (!result) {
            if (!candidateErrors.empty()) candidateErrors += "; ";
            candidateErrors += mdlPath + ": " + result.error().message;
            continue; // The full corpus sweep reports malformed model reasons.
        }
        ++multichoiceBakeSuccesses;
        bodyPart = static_cast<std::size_t>(part - source.mdl.bodyParts.begin());
        partChoiceCount = part->models.size();
        partBase = part->base > 0 ? static_cast<std::uint32_t>(part->base) : 1;
        baked = std::move(*result);
        for (std::size_t i = 0; i < part->models.size(); ++i) {
            const bool hasTriangles = std::ranges::any_of(baked->submeshes, [&](const auto& mesh) {
                return mesh.bodyPart == bodyPart && mesh.choice == i && mesh.indexCount > 0;
            });
            if (part->models[i].verticesCount > 0 && hasTriangles)
                choices.push_back(static_cast<std::uint32_t>(i));
        }
        modelName = mdlPath;
        break;
    }
    INFO("bodygroup model: " << modelName << "; candidate failures: " << candidateErrors
         << "; multichoice models=" << multichoiceModels
         << " bake successes=" << multichoiceBakeSuccesses);
    REQUIRE(baked.has_value());
    REQUIRE(bodyPart < baked->bodyParts.size());
    CHECK(baked->bodyParts[bodyPart].choices.size() == partChoiceCount);
    REQUIRE(choices.size() > 1);
    const auto allIndices = std::accumulate(baked->submeshes.begin(), baked->submeshes.end(),
                                           std::size_t{0}, [](auto sum, const auto& mesh) {
                                               return sum + mesh.indexCount;
                                           });
    for (const auto choice : {choices[0], choices[1]}) {
        const auto body = choice * partBase;
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

TEST_CASE("model bake corpus single-choice indices match mdlpp") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile profile;
    profile.gameDirectory = std::filesystem::path(install) / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    std::size_t eligible = 0, compared = 0, bakeErrors = 0, comparedVertices = 0;
    for (const auto& mdlPath : vfs->walk("models", "*.mdl")) {
        if (eligible == 300) break; // Deterministic canonical-path prefix of eligible models.
        const auto vtxPath = mdlPath.substr(0, mdlPath.size() - 4) + ".dx90.vtx";
        const auto vvdPath = mdlPath.substr(0, mdlPath.size() - 4) + ".vvd";
        const auto mdlBytes = vfs->open(mdlPath);
        const auto vtxBytes = vfs->open(vtxPath);
        const auto vvdBytes = vfs->open(vvdPath);
        if (!mdlBytes || !vtxBytes || !vvdBytes) continue;
        mdlpp::StudioModel source{};
        if (!source.open(*mdlBytes, *vtxBytes, *vvdBytes) || source.mdl.bodyParts.size() != 1 ||
            source.mdl.bodyParts[0].models.size() != 1 || source.vvd.vertices.size() >= 65536) continue;
        ++eligible;
        const auto baked = bakeModel(source);
        if (!baked) { ++bakeErrors; continue; }
        const auto reference = source.processModelData(0);
        std::vector<std::uint32_t> actual, expected;
        for (const auto& range : baked->submeshes)
            actual.insert(actual.end(), baked->indices.begin() + range.firstIndex,
                          baked->indices.begin() + range.firstIndex + range.indexCount);
        for (const auto& mesh : reference.meshes)
            expected.insert(expected.end(), mesh.indices.begin(), mesh.indices.end());
        CAPTURE(mdlPath);
        CHECK(actual == expected);
        // Vertex data too: same count and order, bit-identical position, normal and UV. Compared by bits
        // because some corpus models carry NaN components, which never compare equal as floats.
        REQUIRE(baked->vertices.size() == reference.vertices.size());
        std::size_t vertexMismatches = 0;
        const auto sameBits = [](float a, float b) {
            return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
        };
        for (std::size_t i = 0; i < reference.vertices.size(); ++i) {
            const auto& got = baked->vertices[i];
            const auto& want = reference.vertices[i];
            const bool same = sameBits(got.position.x, want.position[0]) && sameBits(got.position.y, want.position[1]) &&
                              sameBits(got.position.z, want.position[2]) && sameBits(got.normal.x, want.normal[0]) &&
                              sameBits(got.normal.y, want.normal[1]) && sameBits(got.normal.z, want.normal[2]) &&
                              sameBits(got.uv.x, want.uv[0]) && sameBits(got.uv.y, want.uv[1]);
            if (!same) ++vertexMismatches;
        }
        CHECK(vertexMismatches == 0);
        comparedVertices += reference.vertices.size();
        ++compared;
    }
    CHECK(compared > 0);
    MESSAGE("single-choice equality comparisons=" << compared << " vertices compared=" << comparedVertices << " eligible sample=" << eligible
            << " bake errors skipped=" << bakeErrors
            << " (first 300 eligible models in canonical path order)");
}

TEST_CASE("model bake corpus sweep counts failures and measures largest model BVH") {
    const auto* install = std::getenv("APEX_P2CE_DIR");
    if (!install || !*install) { MESSAGE("Skipped: APEX_P2CE_DIR is unset"); return; }
    GameProfile profile;
    profile.gameDirectory = std::filesystem::path(install) / "p2ce";
    JobPool jobs(2);
    auto vfs = Vfs::load(profile, jobs);
    REQUIRE(vfs);
    const auto started = std::chrono::steady_clock::now();
    struct Partial {
        std::size_t models = 0, baked = 0, largestVertices = 0, largestTriangles = 0;
        std::string largestName;
        std::map<std::string, std::size_t> failures;
        std::optional<ModelData> largest;
    };
    const auto paths = vfs->walk("models", "*.mdl");
    const std::size_t workerCount = std::min<std::size_t>(4, std::max<std::size_t>(1, paths.size()));
    std::vector<std::vector<std::string>> batches(workerCount);
    for (std::size_t i = 0; i < paths.size(); ++i) batches[i % workerCount].push_back(paths[i]);
    std::vector<Partial> partials;
    std::size_t completed = 0;
    for (auto& batch : batches) {
        jobs.submit(Priority::Low,
                    [batch = std::move(batch), &vfs](CancelToken) {
            Partial part;
            for (const auto& mdlPath : batch) {
                ++part.models;
                const auto stem = mdlPath.substr(0, mdlPath.size() - 4);
                const auto mdlBytes = vfs->open(mdlPath);
                const auto vtxBytes = vfs->open(stem + ".dx90.vtx");
                const auto vvdBytes = vfs->open(stem + ".vvd");
                if (!mdlBytes || !vtxBytes || !vvdBytes) {
                    ++part.failures["missing model companion"];
                    continue;
                }
                mdlpp::StudioModel source{};
                if (!source.open(*mdlBytes, *vtxBytes, *vvdBytes)) {
                    ++part.failures["mdlpp open failed"];
                    continue;
                }
                auto baked = bakeModel(source);
                if (!baked) {
                    const auto& reason = baked.error().message;
                    if (reason.empty()) {
                        ++part.failures["empty bake reason"];
                    } else {
                        const auto colon = reason.find(':');
                        ++part.failures[reason.substr(0, colon)];
                    }
                    continue;
                }
                ++part.baked;
                const auto triangles = baked->indices.size() / 3;
                if (baked->vertices.size() > part.largestVertices) {
                    part.largestVertices = baked->vertices.size();
                    part.largestTriangles = triangles;
                    part.largestName = mdlPath;
                    part.largest = std::move(*baked);
                }
            }
            return part;
        }, [&](Partial part) {
            partials.push_back(std::move(part));
            ++completed;
        });
    }
    while (completed < workerCount) {
        jobs.drainCompletions();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::size_t models = 0, bakedCount = 0, largestVertices = 0, largestTriangles = 0;
    std::string largestName;
    std::map<std::string, std::size_t> failures;
    std::optional<ModelData> largest;
    for (auto& part : partials) {
        models += part.models;
        bakedCount += part.baked;
        for (const auto& [reason, count] : part.failures) failures[reason] += count;
        if (part.largestVertices > largestVertices) {
            largestVertices = part.largestVertices;
            largestTriangles = part.largestTriangles;
            largestName = std::move(part.largestName);
            largest = std::move(part.largest);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    std::size_t failedCount = 0;
    for (const auto& [reason, count] : failures) failedCount += count;
    CHECK(models == bakedCount + failedCount);
    MESSAGE("corpus models=" << models << " baked=" << bakedCount << " failed=" << failedCount);
    for (const auto& [reason, count] : failures) MESSAGE("failure reason " << reason << "=" << count);
    MESSAGE("largest model=" << largestName << " vertices=" << largestVertices << " triangles=" << largestTriangles);
    MESSAGE("corpus sweep wall time=" << std::chrono::duration<double>(elapsed).count() << " s");

    if (largest && largestTriangles > 0) {
        std::vector<geometry::Bvh::Item> items;
        items.reserve(largestTriangles);
        std::uint32_t triangleId = 0;
        for (std::size_t i = 0; i + 2 < largest->indices.size(); i += 3) {
            AABB box;
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const auto index = largest->indices[i + corner];
                REQUIRE(index < largest->vertices.size());
                box.expand(DVec3(largest->vertices[index].position));
            }
            items.push_back({triangleId++, box});
        }
        geometry::Bvh bvh;
        const auto bvhStarted = std::chrono::steady_clock::now();
        bvh.build(items);
        const auto bvhElapsed = std::chrono::steady_clock::now() - bvhStarted;
        CHECK(bvh.size() == largestTriangles);
        MESSAGE("largest model triangle BVH build time=" << std::chrono::duration<double>(bvhElapsed).count() << " s (geometry::Bvh)");
    } else {
        MESSAGE("largest model triangle BVH build time=unavailable (no baked triangles)");
    }
}
