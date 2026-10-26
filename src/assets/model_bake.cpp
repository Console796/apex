#include "assets/model_bake.hpp"

#include <mdlpp/mdlpp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace apex::assets {
namespace {
constexpr std::int64_t kStudioVertexSize = 48; // sizeof(mstudiovertex_t)

Error bakeError(std::string message) { return {ErrorCode::ParseError, std::move(message)}; }

// mdlpp builds a strip's spans from file offsets and counts without checking them, so a corrupt VTX can
// point them outside the strip group's own vectors.
template <typename T>
bool spanWithin(std::span<T> span, const std::vector<std::remove_const_t<T>>& owner) {
    if (span.empty()) return true;
    const auto first = reinterpret_cast<std::uintptr_t>(span.data());
    const auto begin = reinterpret_cast<std::uintptr_t>(owner.data());
    const auto end = begin + owner.size() * sizeof(T);
    return first >= begin && first <= end && span.size() <= (end - first) / sizeof(T);
}

ModelVertex convertVertex(const mdlpp::VVD::Vertex& vertex) {
    return {{vertex.position[0], vertex.position[1], vertex.position[2]},
            {vertex.normal[0], vertex.normal[1], vertex.normal[2]}, {vertex.uv[0], vertex.uv[1]}};
}

} // namespace

Result<ModelData> bakeModel(const mdlpp::MDL::MDL& mdl, const mdlpp::VTX::VTX& vtx,
                            const mdlpp::VVD::VVD& vvd) {
    try {
        if (vtx.bodyParts.empty()) return std::unexpected(bakeError("empty vtx body parts"));
        if (vtx.numLODs <= 0) return std::unexpected(bakeError("missing lod 0"));
        if (mdl.bodyParts.size() != vtx.bodyParts.size())
            return std::unexpected(bakeError("body part count mismatch"));

        ModelData result;
        result.skins = mdl.skins;
        result.materialDirectories = mdl.materialDirectories;
        result.materialNames.reserve(mdl.materials.size());
        for (const auto& material : mdl.materials) result.materialNames.push_back(material.name);

        if (vvd.fixups.empty()) {
            result.vertices.reserve(vvd.vertices.size());
            for (std::size_t i = 0; i < vvd.vertices.size(); ++i) {
                if (i >= std::numeric_limits<std::uint32_t>::max())
                    return std::unexpected(bakeError("vertex id out of range: model exceeds uint32"));
                result.vertices.push_back(convertVertex(vvd.vertices[i]));
            }
        } else {
            for (const auto& fixup : vvd.fixups) {
                // A fixup serves every LOD up to its own, so LOD 0 takes all of them.
                if (fixup.LOD < 0) continue;
                if (fixup.sourceVertexID < 0 || fixup.vertexCount < 0 ||
                    static_cast<std::uint64_t>(fixup.sourceVertexID) + static_cast<std::uint64_t>(fixup.vertexCount) >
                        vvd.vertices.size()) {
                    return std::unexpected(bakeError("vertex id out of range: VVD fixup"));
                }
                for (std::int32_t i = 0; i < fixup.vertexCount; ++i) {
                    if (result.vertices.size() >= std::numeric_limits<std::uint32_t>::max())
                        return std::unexpected(bakeError("vertex id out of range: model exceeds uint32"));
                    const auto sourceIndex = static_cast<std::size_t>(fixup.sourceVertexID + i);
                    result.vertices.push_back(convertVertex(vvd.vertices.at(sourceIndex)));
                }
            }
        }
        if (result.vertices.size() > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(bakeError("vertex id out of range: model exceeds uint32"));

        result.bodyParts.reserve(mdl.bodyParts.size());
        for (std::size_t bodyIndex = 0; bodyIndex < mdl.bodyParts.size(); ++bodyIndex) {
            const auto& mdlBody = mdl.bodyParts.at(bodyIndex);
            const auto& vtxBody = vtx.bodyParts.at(bodyIndex);
            if (mdlBody.models.size() != vtxBody.models.size())
                return std::unexpected(bakeError("model count mismatch"));
            ModelBodyPart body;
            body.name = mdlBody.name;
            body.base = mdlBody.base > 0 ? static_cast<std::uint32_t>(mdlBody.base) : 1u;
            body.choices.reserve(mdlBody.models.size());

            for (std::size_t choiceIndex = 0; choiceIndex < mdlBody.models.size(); ++choiceIndex) {
                const auto& mdlModel = mdlBody.models.at(choiceIndex);
                const auto& vtxModel = vtxBody.models.at(choiceIndex);
                ModelChoice choice;
                choice.name = mdlModel.name;
                // mstudiomodel_t::vertexindex is a byte offset into the 48-byte vertex array; mesh offsets are counts.
                if (mdlModel.verticesOffset < 0 || mdlModel.verticesCount < 0 ||
                    mdlModel.verticesOffset % kStudioVertexSize != 0)
                    return std::unexpected(bakeError("vertex id out of range: bad model vertex span"));
                const std::int64_t modelVertexBase = mdlModel.verticesOffset / kStudioVertexSize;
                body.choices.push_back(std::move(choice));
                auto& bakedChoice = body.choices.back();

                if (vtxModel.modelLODs.empty())
                    return std::unexpected(bakeError("missing lod 0"));
                const auto& lod = vtxModel.modelLODs.at(0);
                if (lod.meshes.size() != mdlModel.meshes.size())
                    return std::unexpected(bakeError("mesh count mismatch"));
                for (std::size_t meshIndex = 0; meshIndex < mdlModel.meshes.size(); ++meshIndex) {
                    const auto& mdlMesh = mdlModel.meshes.at(meshIndex);
                    const auto& vtxMesh = lod.meshes.at(meshIndex);
                    if (result.indices.size() > std::numeric_limits<std::uint32_t>::max())
                        return std::unexpected(bakeError("index count exceeds uint32"));
                    ModelSubmesh submesh;
                    submesh.firstIndex = static_cast<std::uint32_t>(result.indices.size());
                    submesh.bodyPart = static_cast<std::uint32_t>(bodyIndex);
                    submesh.choice = static_cast<std::uint32_t>(choiceIndex);
                    submesh.mesh = static_cast<std::uint32_t>(meshIndex);
                    submesh.material = mdlMesh.material;

                    const auto appendVertex = [&](const mdlpp::VTX::StripGroup& group, std::uint16_t stripIndex) -> Result<void> {
                        if (stripIndex >= group.vertices.size())
                            return std::unexpected(bakeError("vertex id out of range: strip vertex"));
                        const auto meshVertex = group.vertices.at(stripIndex).meshVertexID;
                        const std::int64_t sourceIndex = static_cast<std::int64_t>(meshVertex) + mdlMesh.verticesOffset +
                                                         modelVertexBase;
                        if (sourceIndex < 0 || static_cast<std::uint64_t>(sourceIndex) >= result.vertices.size())
                            return std::unexpected(bakeError("vertex id out of range"));
                        const auto bakedIndex = static_cast<std::uint32_t>(sourceIndex);
                        if (result.indices.size() >= std::numeric_limits<std::uint32_t>::max())
                            return std::unexpected(bakeError("index count exceeds uint32"));
                        result.indices.push_back(bakedIndex);
                        const auto position = result.vertices.at(bakedIndex).position;
                        bakedChoice.bounds.expand(DVec3(position));
                        result.bounds.expand(DVec3(position));
                        return {};
                    };
                    for (const auto& group : vtxMesh.stripGroups) {
                        for (const auto& strip : group.strips) {
                            if (!spanWithin(strip.indices, group.indices))
                                return std::unexpected(bakeError("strip range out of bounds"));
                            if (strip.flags & mdlpp::VTX::Strip::FLAG_IS_TRILIST) {
                                for (std::size_t i = 0; i + 2 < strip.indices.size(); i += 3) {
                                    for (const auto k : {i, i + 2, i + 1}) {
                                        auto added = appendVertex(group, strip.indices[k]);
                                        if (!added) return std::unexpected(added.error());
                                    }
                                }
                            } else {
                                for (std::size_t i = 0; i + 2 < strip.indices.size(); ++i) {
                                    const auto order = (i & 1u) ? std::array<std::size_t, 3>{i, i + 2, i + 1}
                                                               : std::array<std::size_t, 3>{i + 1, i + 2, i};
                                    for (const auto k : order) {
                                        auto added = appendVertex(group, strip.indices[k]);
                                        if (!added) return std::unexpected(added.error());
                                    }
                                }
                            }
                        }
                    }
                    const auto count = result.indices.size() - submesh.firstIndex;
                    if (count > std::numeric_limits<std::uint32_t>::max())
                        return std::unexpected(bakeError("index count exceeds uint32"));
                    submesh.indexCount = static_cast<std::uint32_t>(count);
                    result.submeshes.push_back(submesh);
                }
                if (bakedChoice.bounds.empty()) {
                    const auto begin = static_cast<std::size_t>(modelVertexBase);
                    const auto end = begin + static_cast<std::size_t>(mdlModel.verticesCount);
                    if (end <= result.vertices.size()) {
                        for (std::size_t i = begin; i < end; ++i)
                            bakedChoice.bounds.expand(DVec3(result.vertices.at(i).position));
                    }
                }
                result.bounds.expand(bakedChoice.bounds);
            }
            result.bodyParts.push_back(std::move(body));
        }
        if (result.bounds.empty()) {
            result.bounds.expand(DVec3{mdl.hullMin[0], mdl.hullMin[1], mdl.hullMin[2]});
            result.bounds.expand(DVec3{mdl.hullMax[0], mdl.hullMax[1], mdl.hullMax[2]});
        }
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected(bakeError("mdlpp exception: " + std::string(exception.what())));
    } catch (...) {
        return std::unexpected(bakeError("mdlpp exception: unknown exception"));
    }
}

Result<ModelData> bakeModel(const mdlpp::StudioModel& model) {
    return bakeModel(model.mdl, model.vtx, model.vvd);
}

std::vector<std::uint32_t> modelBodyChoices(const ModelData& model, std::uint32_t body) {
    std::vector<std::uint32_t> choices;
    choices.reserve(model.bodyParts.size());
    for (const auto& part : model.bodyParts) {
        if (part.choices.empty()) {
            choices.push_back(0);
        } else {
            const std::uint32_t base = part.base ? part.base : 1;
            choices.push_back((body / base) % static_cast<std::uint32_t>(part.choices.size()));
        }
    }
    return choices;
}

std::vector<ModelDrawRange> modelDrawRanges(const ModelData& model, std::uint32_t body, std::uint32_t skin) {
    const auto choices = modelBodyChoices(model, body);
    const auto family = skin < model.skins.size() ? skin : 0;
    std::vector<ModelDrawRange> ranges;
    for (const auto& submesh : model.submeshes) {
        if (submesh.bodyPart >= choices.size() || choices[submesh.bodyPart] != submesh.choice) continue;
        std::int32_t material = submesh.material;
        if (family < model.skins.size() && material >= 0 &&
            static_cast<std::size_t>(material) < model.skins[family].size()) {
            material = model.skins[family][static_cast<std::size_t>(material)];
        }
        ranges.push_back({submesh.firstIndex, submesh.indexCount, material});
    }
    return ranges;
}

} // namespace apex::assets
