#include "document/entity_helpers.hpp"
#include "render/render_mirror.hpp"

#include "core/log.hpp"
#include "assets/model_pose.hpp"
#include "document/entity_transform.hpp"
#include "geometry/solid_geometry.hpp"
#include "render/helper_shapes.hpp"
#include "render/solid_mesh.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <charconv>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

using document::Document;
using document::NodeHandle;
using document::NodeId;

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Stable pastel colour per material for Flat mode.
Vec4 flatColour(const std::string& material) {
    const std::size_t h = std::hash<std::string>{}(material);
    const auto channel = [&](int shift) { return 0.35f + 0.55f * static_cast<float>((h >> shift) & 0xFF) / 255.0f; };
    return {channel(0), channel(8), channel(16), 1.0f};
}

struct GpuBatch {
    std::string material;
    Buffer vertices, indices, edges;
    std::uint32_t indexCount = 0, edgeCount = 0;
    std::vector<FaceRange> faces;
};

struct SolidState {
    std::shared_ptr<const kv1::Node> block; // Immutable snapshot owned by the document node.
    std::uint64_t version = 0;              // Latest requested mesh.
    bool inFlight = false;
    bool hidden = false;
    bool meshed = false;
    JobHandle job;
    DVec3 origin{0.0};
    AABB bounds;          // World bounds of the last finished mesh; brush-entity helpers centre on these.
    bool bounded = false;
    std::vector<GpuBatch> batches;
    std::vector<std::string> materials;
};

struct ModelMesh {
    std::string path;     // ModelCache key.
    std::string sequence; // Lower-case defaultanim; empty draws the cache's static pose.
    std::shared_ptr<const assets::ModelData> data;
    Buffer vertices, indices, edges;
    std::map<std::int32_t, std::string> materials;
};

constexpr float kSpriteHalfSize = 8.0f; // World units; Hammer draws icons about 16 across.

std::uint32_t entityUint(const kv1::Node& block, std::string_view key) {
    const auto* node = block.find(key);
    const auto text = node ? node->decodedValue().value_or("") : "";
    std::uint32_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

// models map key: the path alone, or path and defaultanim when the entity names a sequence. The game
// spawns props in that sequence's first frame (prop_testchamber_door's leaves exist only in idleclose).
std::string modelMeshKey(const std::string& path, const std::string& sequence) {
    return sequence.empty() ? path : path + '\n' + sequence;
}

struct EntityState {
    std::string model; // models map key (modelMeshKey), not the bare path.
    std::uint32_t skin = 0, body = 0;
    DMat4 transform{1.0};
    DVec3 position{0.0}, mins{-8.0}, maxs{8.0};
    DMat3 basis{1.0};
    Vec4 colour{0.9f, 0.4f, 0.9f, 1.0f};
    // Helper inputs and output (D5). The block is shared with the document, so keeping it is cheap.
    std::shared_ptr<const kv1::Node> block;
    document::EntityPose pose;
    const assets::EntityClass* cls = nullptr;
    std::string targetname;         // As written, for labels.
    std::string instanceFile;
    std::string sprite;             // Editor icon material of the class (iconsprite); empty when it has none.
    std::string nameKey;            // Lower case; empty when unnamed.
    HelperShapes helpers;
    std::set<std::string> watching; // Names peersNamed looked up during the last evaluation.
    // Brush entities have no box: their origin is the centre of their solids' bounds (or an `origin`
    // key), known once the solids have meshed.
    bool brush = false;
    std::vector<NodeId> brushSolids;
    bool cubemap = false; // env_cubemap: a reflection probe at `position`.
};

struct MaterialState {
    TextureSlot slot = kCheckerboardSlot;
    TextureSlot bumpSlot = kNoTexture;
    TextureSlot mraoSlot = kNoTexture;
    TextureSlot phongExponentSlot = kNoTexture;
    TextureSlot envmapCube = kNoTexture; // Owned cube slot of a named $envmap.
    TextureSlot envmapMaskSlot = kNoTexture;
    TextureSlot selfIllumMaskSlot = kNoTexture;
    TextureSize size;
    bool loading = false;
    std::size_t textureBytes = 0;
    AlphaMode alpha = AlphaMode::Opaque; // From $translucent / $alphatest once the VMT has loaded.
    bool sky = false;                   // %compilesky once loaded.
    bool nodraw = false;                 // %compilenodraw once loaded; see isNodraw().
    std::set<NodeId> users;
};

// toolsnodraw is recognised by name so it hides before its VMT has loaded.
bool isNodraw(std::string_view name, const MaterialState* state) {
    constexpr std::string_view kName = "tools/toolsnodraw";
    const bool named = name.size() == kName.size() &&
                       std::ranges::equal(name, kName, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
    return named || (state && state->nodraw);
}

// Name fallback also works before a VMT is available, including toolsskybox2d and HDR variants.
bool isSkyMaterial(std::string_view name, bool compileSky = false) {
    return compileSky || lower(name).starts_with("tools/toolsskybox");
}

bool isToolFace(std::string_view name, const MaterialState* state) {
    return lower(name).starts_with("tools/") && !isSkyMaterial(name, state && state->sky);
}

bool blocksSky(std::string_view name) {
    const auto key = lower(name);
    // VRAD's trigger/clip contents do not block light. Keep nodraw (including %compilenodraw):
    // it suppresses drawing, not light visibility. Unknown tools stay solid conservatively.
    return key != "tools/toolstrigger" && key != "tools/toolsclip" &&
           key != "tools/toolsplayerclip" && key != "tools/toolsnpcclip";
}

} // namespace

struct RenderMirror::Impl {
    Context& context;
    TextureHeap& heap;
    JobPool& jobs;
    assets::MaterialCache& cache;
    const assets::EntityClassTable* classes;
    assets::ModelCache* modelCache = nullptr;
    TextureSlot errorSlot = kCheckerboardSlot;
    std::function<bool(const std::string&)> modelMaterialExists;
    std::map<std::string, ModelMesh> models;
    std::vector<document::InstanceContent> instances;
    std::map<std::shared_ptr<const Document>, std::unique_ptr<RenderMirror>> instanceMirrors;
    document::InstanceVisibility instanceVisibility = document::InstanceVisibility::Normal;
    std::size_t drawnInstanceSolids = 0;
    // Set by the parent on a mirror that draws an instance's contents: the instances inside it
    // have no resolution state of their own, so their path labels follow the owner's inspection.
    bool drawsInstanceContents = false;
    bool ownerInspected = false;

    std::map<NodeId, SolidState> solids;
    std::map<Handle<document::Solid>, NodeId> solidIds;
    std::map<Handle<document::Entity>, NodeId> entityIds;
    std::map<NodeId, EntityState> entities;
    std::vector<lighting::Light> lights;
    bool lightsDirty = true;
    std::shared_ptr<const lighting::SkyVisibilityGrid> skyGrid;
    JobHandle skyJob;
    std::uint64_t skyRevision = 0, skyRequested = 1;
    bool skyBuilding = false, skyWanted = false, skyUsedSincePump = false, skyHasAmbient = false;
    std::chrono::steady_clock::time_point skyChanged = std::chrono::steady_clock::now();
    std::map<NodeId, std::string> skyEntityClasses;
    std::map<std::shared_ptr<const Document>, std::uint64_t> skyInstanceRevisions;

    void invalidateSky() {
        ++skyRequested;
        skyChanged = std::chrono::steady_clock::now();
    }

    std::map<std::string, MaterialState, std::less<>> materials;
    TextureSize placeholderSize;
    CubemapSource cubemapSource;
    std::map<std::array<long long, 3>, TextureSlot> probeCubes; // Rounded origin -> cube slot.

    Buffer entityBuffer;
    std::uint32_t entityCount = 0;
    Buffer spriteBuffer;            // One SpriteInstance per entity drawn as its billboarded icon.
    std::uint32_t spriteCount = 0;
    bool iconNearest = false;       // Bit 31 of a sprite's texture selects the nearest sampler.
    DVec3 entityOrigin{0.0};
    bool entitiesDirty = false;
    // Helper dependencies: helpers that name other entities (line, cylinder, keyframe chains) are
    // re-evaluated only when an entity carrying one of the names they looked up changes.
    std::map<std::string, std::set<NodeId>, std::less<>> byName;
    std::map<std::string, std::set<NodeId>, std::less<>> watchers;
    std::set<std::string> touchedNames;
    std::set<NodeId> helperDirty;
    std::map<NodeId, std::map<std::string, std::string>> helperPreview;
    std::map<NodeId, NodeId> brushOwner; // Solid -> brush entity.
    std::set<NodeId> brushDirty;         // Brush entities whose solids meshed or went away.
    bool redraw = true;
    bool uploaded = false; // Texture uploads awaiting heap.flush().

    std::vector<Buffer> garbage;
    MirrorStats stats;
    Highlight highlight;
    std::set<NodeId> selected;
    std::set<document::SideId> selectedSides;
    PreviewTransforms preview;
    struct Hierarchy {
        NodeId parent;
        std::vector<NodeId> children;
    };
    std::unordered_map<NodeId, Hierarchy> hierarchy;
    std::map<NodeHandle, NodeId> hierarchyIds;
    std::unordered_map<NodeId, DMat4> effectivePreview;
    bool previewDirty = false;
    struct PreviewState {
        NodeId node = 0; // Zero is a ghost, never an indexed document node.
        PreviewGhost shape;
        SolidState mesh;
    };
    std::vector<PreviewState> previewGeometry;
    std::size_t previewCursor = 0;
    std::optional<std::uint64_t> previewRevision;
    std::chrono::nanoseconds previewBudget = std::chrono::milliseconds(2);
    // Seeded so the first tick of a drag reserves upload time too (about 6 us per box measured on
    // an RTX 4060 Ti at the 0.3 release); a zero seed let that tick run to 4.8 ms with 500 solids.
    std::chrono::nanoseconds previewFlushPerSolid = std::chrono::microseconds(10);

    void clearGeometry() {
        for (auto& state : previewGeometry) retire(state.mesh);
        previewGeometry.clear();
        previewCursor = 0;
        previewRevision.reset();
    }

    // The tick's upload submission counts against the budget: its cost per solid is learned from
    // the previous tick and reserved while meshing. At least one solid is meshed per tick.
    void meshPreviews() {
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        std::size_t meshed = 0;
        while (previewCursor < previewGeometry.size()) {
            auto& state = previewGeometry[previewCursor++];
            auto poly = state.shape.poly;
            for (auto& face : poly.faces) {
                if (!state.shape.material.empty()) face.material = state.shape.material;
                resolveMaterial(lower(face.material));
            }
            auto mesh = meshSolid(poly, [&](std::string_view name) { return materials.find(name)->second.size; });
            uploadMesh(state.mesh, std::move(mesh), false);
            ++meshed;
            if (Clock::now() - start + previewFlushPerSolid * meshed >= previewBudget) break;
        }
        if (!meshed) return;
        redraw = true;
        // One submission per tick instead of three per batch; buffers are drawn only after this.
        const auto flushStart = Clock::now();
        if (auto flushed = heap.uploadRing().flush(); !flushed)
            log::error("render mirror: preview upload: {}", flushed.error().what());
        previewFlushPerSolid = (Clock::now() - flushStart) / meshed;
    }

    void resolvePreview() {
        if (!previewDirty) return;
        previewDirty = false;
        effectivePreview.clear();
        if (preview.empty()) return;
        const std::unordered_map<NodeId, DMat4> direct(preview.begin(), preview.end());
        const auto walk = [&](const auto& self, NodeId id, const DMat4* inherited) -> void {
            const auto found = direct.find(id);
            const DMat4* matrix = found == direct.end() ? inherited : &found->second;
            if (matrix) effectivePreview.emplace(id, *matrix);
            const auto node = hierarchy.find(id);
            if (node != hierarchy.end())
                for (const auto child : node->second.children) self(self, child, matrix);
        };
        for (const auto& [id, node] : hierarchy)
            if (!hierarchy.contains(node.parent)) walk(walk, id, nullptr);
    }

    Impl(Context& c, TextureHeap& h, JobPool& j, assets::MaterialCache& m, const assets::EntityClassTable* k,
         assets::ModelCache* modelsCache, std::function<bool(const std::string&)> exists)
        : context(c), heap(h), jobs(j), cache(m), classes(k), modelCache(modelsCache),
          modelMaterialExists(std::move(exists)) {
        const auto placeholder = cache.placeholder();
        if (placeholder && !placeholder->texture.mips.empty()) {
            placeholderSize = {std::max<int>(1, placeholder->texture.mips[0].width),
                               std::max<int>(1, placeholder->texture.mips[0].height)};
        }
    }

    ~Impl() {
        skyJob.cancel();
        for (auto& [id, solid] : solids) solid.job.cancel();
        context.waitIdle();
        for (auto& [name, material] : materials) releaseTextures(material);
        for (const auto& [origin, cube] : probeCubes) heap.releaseCube(cube);
        if (errorSlot != kCheckerboardSlot) heap.release(errorSlot);
        heap.collectGarbage();
    }

    // Releases every heap slot the material owns and resets it to the placeholder.
    void releaseTextures(MaterialState& state) {
        if (state.slot != kCheckerboardSlot) heap.release(state.slot);
        for (TextureSlot* slot : {&state.bumpSlot, &state.mraoSlot, &state.phongExponentSlot,
                                  &state.envmapMaskSlot, &state.selfIllumMaskSlot}) {
            if (*slot != kNoTexture) heap.release(*slot);
            *slot = kNoTexture;
        }
        if (state.envmapCube != kNoTexture) heap.releaseCube(state.envmapCube);
        state.envmapCube = kNoTexture;
        state.slot = kCheckerboardSlot;
    }

    // Probe cubes come from cubemapSource, once per rounded origin; slot 0 when it has none.
    TextureSlot probeCube(DVec3 origin) {
        if (!cubemapSource) return kDefaultCubeSlot;
        const std::array<long long, 3> key{std::llround(origin.x), std::llround(origin.y), std::llround(origin.z)};
        if (const auto found = probeCubes.find(key); found != probeCubes.end()) return found->second;
        TextureSlot cube = kDefaultCubeSlot;
        if (auto texture = cubemapSource(origin)) {
            if (auto created = heap.uploadCube(*texture)) cube = *created;
            else log::warn("render mirror: cubemap at ({}, {}, {}) failed: {}", key[0], key[1], key[2],
                           std::string_view(created.error().message));
        }
        probeCubes.emplace(key, cube);
        return cube;
    }

    void loadDefaultCube() {
        if (heap.defaultCubeLoaded()) return;
        auto texture = cache.loadTexture("engine/defaultcubemap");
        if (!texture) return; // The mid-grey fallback stays.
        if (auto set = heap.setDefaultCube(*texture); !set)
            log::warn("render mirror: engine/defaultcubemap: {}", std::string_view(set.error().message));
    }

    // Destroys retired buffers once no frame can still use them.
    void collectGarbage() {
        if (garbage.empty()) return;
        context.waitIdle();
        garbage.clear();
    }

    void retire(SolidState& solid) {
        for (GpuBatch& batch : solid.batches) {
            garbage.push_back(std::move(batch.vertices));
            garbage.push_back(std::move(batch.indices));
            garbage.push_back(std::move(batch.edges));
        }
        solid.batches.clear();
        solid.meshed = false;
    }

    // --- Materials -------------------------------------------------------------------------

    void uploadMaterial(const std::string& name, MaterialState& state, const assets::CachedMaterial& material) {
        const auto skyFlag = material.material.compileFlags.find("%compilesky");
        const bool sky = skyFlag != material.material.compileFlags.end() && !skyFlag->second.empty() && skyFlag->second != "0";
        if (state.sky != sky) {
            state.sky = sky;
            invalidateSky();
        }
        const assets::Texture& texture = material.texture;
        const auto format = vulkanFormat(texture.format);
        if (!format || texture.mips.empty()) return;
        const assets::Material& vmt = material.material;
        state.alpha = vmt.translucent ? AlphaMode::Blend : vmt.alphaTest ? AlphaMode::Test : AlphaMode::Opaque;
        if (const auto flag = vmt.compileFlags.find("%compilenodraw"); flag != vmt.compileFlags.end()) {
            state.nodraw = !flag->second.empty() && flag->second != "0";
        }
        auto slot = heap.upload(texture);
        if (!slot) {
            log::warn("render mirror: texture upload for '{}' failed: {}", std::string_view(name),
                      std::string_view(slot.error().message));
            return;
        }
        releaseTextures(state);
        state.slot = *slot;
        std::uint32_t flags = texture.format == assets::TextureFormat::Rgba32Float ||
            texture.format == assets::TextureFormat::Bc6Signed || texture.format == assets::TextureFormat::Bc6Unsigned ?
            kMaterialLinearBase : 0u;
        const auto shader = lower(vmt.shader);
        if (material.bumpTexture && (shader == "lightmappedgeneric" || shader == "vertexlitgeneric" ||
                                      shader == "worldvertextransition" || shader == "pbr")) {
            if (auto bump = heap.upload(*material.bumpTexture)) {
                state.bumpSlot = *bump;
                flags |= kMaterialHasBump | (vmt.ssbump() ? kMaterialSsbump : 0u);
                if (material.bumpTexture->format == assets::TextureFormat::Bc5) flags |= kMaterialTwoChannelNormal;
            }
        }
        GpuMaterial record{state.slot, state.bumpSlot, flags, kNoTexture};
        if (shader == "pbr") {
            record.flags |= kMaterialPbr;
            if (material.mraoTexture) {
                if (auto data = heap.upload(*material.mraoTexture)) state.mraoSlot = *data;
            }
            record.mraoSlot = state.mraoSlot;
            record.mraoScale = Vec4(Vec3(vmt.mraoScale()), 0);
        }
        if (shader == "vertexlitgeneric" && vmt.phong()) {
            record.flags |= kMaterialPhong | (vmt.baseMapAlphaPhongMask() ? kMaterialBaseAlphaPhongMask : 0u);
            if (material.phongExponentTexture) {
                if (auto data = heap.upload(*material.phongExponentTexture)) state.phongExponentSlot = *data;
            }
            record.phongExponentSlot = state.phongExponentSlot;
            record.phongParams = Vec4(float(vmt.phongExponent()), float(vmt.phongBoost()), 0, 0);
            record.phongFresnel = Vec4(Vec3(vmt.phongFresnelRanges()), 0);
            record.phongTint = Vec4(Vec3(vmt.phongTint()), 0);
        }
        const bool lit = shader == "lightmappedgeneric" || shader == "vertexlitgeneric" ||
                         shader == "worldvertextransition" || shader == "pbr";
        std::size_t envmapBytes = 0;
        if (lit && !vmt.envmap().empty()) {
            if (vmt.envmapProbe()) {
                record.flags |= kMaterialEnvmap | kMaterialEnvmapProbe;
            } else if (material.envmapTexture) {
                if (auto cube = heap.uploadCube(*material.envmapTexture)) {
                    state.envmapCube = record.envmapSlot = *cube;
                    record.flags |= kMaterialEnvmap;
                    envmapBytes += material.envmapTexture->byteSize();
                } else {
                    log::warn("render mirror: envmap of '{}' failed: {}", std::string_view(name),
                              std::string_view(cube.error().message));
                }
            }
            if ((record.flags & kMaterialEnvmap) != 0u) {
                // $envmaptint is a gamma-space colour, decoded by 2.2 like base textures (fitted to captures:
                // the linear reading left black_floor_metal_001a's reflection about 6x too bright).
                const DVec3 tint = glm::pow(glm::max(vmt.envmapTint(), DVec3(0)), DVec3(2.2));
                record.envmapTint = Vec4(Vec3(tint), float(vmt.envmapLightScale()));
                record.envmapParams = Vec4(float(vmt.envmapContrast()), float(vmt.envmapSaturation()), 0, 0);
                // PBR masks by its own roughness and AO; the mask sources belong to the other shaders.
                if (shader != "pbr") {
                    if (material.envmapMaskTexture) {
                        if (auto mask = heap.upload(*material.envmapMaskTexture)) {
                            state.envmapMaskSlot = record.envmapMaskSlot = *mask;
                            envmapBytes += material.envmapMaskTexture->byteSize();
                        }
                    }
                    if (vmt.normalMapAlphaEnvmapMask()) record.flags |= kMaterialNormalAlphaEnvmapMask;
                    if (vmt.baseAlphaEnvmapMask()) record.flags |= kMaterialBaseAlphaEnvmapMask;
                }
            }
        }
        if (lit && vmt.selfIllum()) {
            record.flags |= kMaterialSelfIllum;
            record.selfIllumTint = Vec4(Vec3(vmt.selfIllumTint()), 0);
            if (material.selfIllumMaskTexture) {
                if (auto mask = heap.upload(*material.selfIllumMaskTexture)) {
                    state.selfIllumMaskSlot = record.selfIllumMaskSlot = *mask;
                    envmapBytes += material.selfIllumMaskTexture->byteSize();
                }
            }
        }
        heap.setMaterial(state.slot, record);
        state.textureBytes = texture.byteSize() + (material.bumpTexture ? material.bumpTexture->byteSize() : 0) +
            (material.mraoTexture ? material.mraoTexture->byteSize() : 0) +
            (material.phongExponentTexture ? material.phongExponentTexture->byteSize() : 0) + envmapBytes;
        state.size = {texture.mips[0].width, texture.mips[0].height, vmt.baseTextureTransform};
        uploaded = true;
        ++stats.texturesUploaded;
        redraw = true;
    }

    MaterialState& resolveMaterial(const std::string& name) {
        auto [it, inserted] = materials.try_emplace(name);
        if (!inserted) return it->second;
        it->second.size = placeholderSize;
        const auto material = cache.request(name);
        switch (cache.state(name)) {
        case assets::MaterialState::Ready:
            if (material) uploadMaterial(name, it->second, *material);
            break;
        case assets::MaterialState::Failed: break;
        default: it->second.loading = true; break;
        }
        return it->second;
    }

    void pumpMaterials() {
        std::vector<NodeId> remesh;
        for (auto& [name, state] : materials) {
            if (!state.loading) continue;
            const auto current = cache.state(name);
            if (current == assets::MaterialState::Loading) continue;
            state.loading = false;
            if (current != assets::MaterialState::Ready) continue;
            entitiesDirty = true; // An icon that just arrived swaps its entities' boxes for sprites.
            const TextureSize before = state.size;
            if (const auto material = cache.request(name)) uploadMaterial(name, state, *material);
            if (!(before == state.size)) {
                remesh.insert(remesh.end(), state.users.begin(), state.users.end());
                for (auto& previewState : previewGeometry) retire(previewState.mesh);
                previewCursor = 0;
            }
        }
        for (const NodeId id : remesh) requestMesh(id);
        if (uploaded) {
            if (auto flushed = heap.flush(); !flushed) {
                log::warn("render mirror: texture flush failed: {}", std::string_view(flushed.error().message));
            }
            uploaded = false;
        }
    }

    // --- Solids ----------------------------------------------------------------------------

    void requestMesh(NodeId id) {
        const auto it = solids.find(id);
        if (it == solids.end()) return;
        SolidState& solid = it->second;
        ++solid.version;
        solid.job.cancel();
        solid.inFlight = false;
        for (const std::string& old : solid.materials) {
            if (const auto m = materials.find(old); m != materials.end()) m->second.users.erase(id);
        }
        solid.materials = solidMaterials(*solid.block);
        if (solid.hidden) {
            retire(solid);
            redraw = true;
            return;
        }
        std::map<std::string, TextureSize, std::less<>> sizes;
        for (const std::string& name : solid.materials) {
            MaterialState& state = resolveMaterial(name);
            state.users.insert(id);
            sizes.emplace(name, state.size);
        }
        kv1::Node copy = *solid.block; // D12: the worker owns a private copy.
        solid.inFlight = true;
        ++stats.jobsSubmitted;
        const std::uint64_t version = solid.version;
        solid.job = jobs.submit(
            Priority::Normal,
            [block = std::move(copy), sizes = std::move(sizes)](CancelToken) mutable -> Result<SolidMesh> {
                return meshSolid(block, [&](std::string_view name) {
                    const auto found = sizes.find(name);
                    return found == sizes.end() ? TextureSize{} : found->second;
                });
            },
            [this, id, version](Result<SolidMesh> mesh) { complete(id, version, std::move(mesh)); });
    }

    Result<Buffer> upload(const auto& data, VkBufferUsageFlags usage, bool flush = true) {
        return uploadBuffer(context, heap.uploadRing(), std::as_bytes(std::span{data}), usage, flush);
    }

    void complete(NodeId id, std::uint64_t version, Result<SolidMesh> mesh) {
        const auto it = solids.find(id);
        if (it == solids.end() || it->second.version != version) return; // Stale or removed.
        SolidState& solid = it->second;
        solid.inFlight = false;
        ++stats.solidsRemeshed;
        redraw = true;
        retire(solid);
        if (mesh) {
            solid.bounds = mesh->bounds;
            solid.bounded = true;
            noteBrushSolid(id);
        }
        if (!mesh) {
            ++stats.meshFailures;
            log::warn("render mirror: solid {}: {}", static_cast<unsigned long long>(id),
                      std::string_view(mesh.error().message));
            return;
        }
        uploadMesh(solid, std::move(mesh));
    }

    // flush false leaves the copies recorded; meshPreviews flushes once per tick.
    void uploadMesh(SolidState& solid, Result<SolidMesh> mesh, bool flush = true) {
        if (!mesh) {
            log::warn("render mirror: preview mesh: {}", std::string_view(mesh.error().message));
            return;
        }
        solid.origin = mesh->origin;
        for (BatchMesh& batch : mesh->batches) {
            if (batch.indices.empty()) continue;
            auto vertices = upload(batch.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, flush);
            auto indices = upload(batch.indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, flush);
            auto edges = upload(batch.edges, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, flush);
            if (!vertices || !indices || !edges) {
                log::error("render mirror: mesh upload failed");
                // Copies into the buffers that did upload may still be recorded and unsubmitted.
                for (auto* buffer : {&vertices, &indices, &edges})
                    if (*buffer) garbage.push_back(std::move(**buffer));
                retire(solid);
                return;
            }
            GpuBatch gpu;
            gpu.material = batch.material;
            gpu.faces = std::move(batch.faces);
            gpu.vertices = std::move(*vertices);
            gpu.indices = std::move(*indices);
            gpu.edges = std::move(*edges);
            gpu.indexCount = static_cast<std::uint32_t>(batch.indices.size());
            gpu.edgeCount = static_cast<std::uint32_t>(batch.edges.size());
            solid.batches.push_back(std::move(gpu));
        }
        solid.meshed = true;
    }

    void removeSolid(NodeId id) {
        const auto it = solids.find(id);
        if (it == solids.end()) return;
        it->second.job.cancel();
        noteBrushSolid(id);
        for (const std::string& name : it->second.materials) {
            if (const auto m = materials.find(name); m != materials.end()) m->second.users.erase(id);
        }
        retire(it->second);
        solids.erase(it);
        ++stats.solidsFreed;
        redraw = true;
    }

    // --- Entities --------------------------------------------------------------------------

    static bool hidden(const Document& doc, const document::Node& node) {
        for (NodeId parent = node.parent; parent != 0;) {
            const auto handle = doc.handle(parent);
            const document::Node* p = handle ? doc.get(*handle) : nullptr;
            if (!p) return false;
            if (p->block && p->block->key() == "hidden") return true;
            parent = p->parent;
        }
        return false;
    }

    void updateEntity(const Document& doc, Handle<document::Entity> handle) {
        lightsDirty = true;
        const document::Entity* entity = doc.get(handle);
        if (!entity) return;
        entityIds[handle] = entity->id;
        eraseEntity(entity->id);
        entitiesDirty = true;
        if (!entity->block || hidden(doc, *entity)) return;
        std::vector<NodeId> brushSolids;
        for (const NodeId child : entity->children) {
            const auto childHandle = doc.handle(child);
            if (childHandle && std::holds_alternative<Handle<document::Solid>>(*childHandle)) brushSolids.push_back(child);
        }
        std::string classname;
        if (const kv1::Node* c = entity->block->find("classname")) {
            if (auto text = c->decodedValue()) classname = *text;
        }
        if (lower(classname) == "worldspawn") return;
        EntityState state;
        if (lower(classname) == "func_instance") {
            if (const auto* file = entity->block->find("file")) state.instanceFile = file->decodedValue().value_or("");
        }
        const auto pose = document::entityPose(*entity->block);
        state.transform = document::entityTransform(pose);
        state.skin = entityUint(*entity->block, "skin");
        state.body = entityUint(*entity->block, "body");
        state.position = pose.origin;
        state.basis = pose.basis;
        state.block = entity->block;
        state.pose = pose;
        state.brush = !brushSolids.empty();
        state.cubemap = lower(classname) == "env_cubemap";
        for (const NodeId solid : brushSolids) brushOwner[solid] = entity->id;
        state.brushSolids = std::move(brushSolids);
        if (const kv1::Node* name = entity->block->find("targetname")) {
            if (auto text = name->decodedValue()) {
                state.targetname = *text;
                state.nameKey = lower(*text);
            }
        }
        if (classes) {
            if (const assets::EntityClass* cls = classes->find(classname)) {
                state.cls = cls;
                if (cls->mins) state.mins = {(*cls->mins)[0], (*cls->mins)[1], (*cls->mins)[2]};
                if (cls->maxs) state.maxs = {(*cls->maxs)[0], (*cls->maxs)[1], (*cls->maxs)[2]};
                state.sprite = spriteMaterial(cls->iconSprite);
                if (!state.sprite.empty()) resolveMaterial(state.sprite);
                if (cls->color) {
                    state.colour = {static_cast<float>((*cls->color)[0]) / 255.0f,
                                    static_cast<float>((*cls->color)[1]) / 255.0f,
                                    static_cast<float>((*cls->color)[2]) / 255.0f, 1.0f};
                }
            }
        }
        if (modelCache) {
            const auto name = document::entityDisplayModel(*entity->block, state.cls);
            if (!name.empty()) {
                const auto path = assets::modelPath(name);
                std::string sequence;
                if (const auto* anim = entity->block->find("defaultanim")) sequence = lower(anim->decodedValue().value_or(""));
                state.model = modelMeshKey(path, sequence);
                auto& mesh = models.try_emplace(state.model).first->second;
                mesh.path = path;
                mesh.sequence = sequence;
                modelCache->request(path);
            }
        }
        if (!state.nameKey.empty()) {
            byName[state.nameKey].insert(entity->id);
            touchedNames.insert(state.nameKey);
        }
        placeBrush(state);
        entities[entity->id] = std::move(state);
        helperDirty.insert(entity->id);
    }

    // The material name of an iconsprite value ("editor/logic_relay.vmt"): no materials/ prefix, no extension.
    static std::string spriteMaterial(std::string_view value) {
        std::string name = lower(value);
        std::ranges::replace(name, '\\', '/');
        if (name.starts_with("materials/")) name.erase(0, 10);
        if (name.ends_with(".vmt")) name.resize(name.size() - 4);
        return name;
    }

    // Point entities without a model show their class icon as a camera-facing sprite once its texture is
    // resident (the box stands in while it loads, and for classes or icons that never resolve).
    bool drawsSprite(const EntityState& e) const {
        if (e.brush || e.sprite.empty() || (!e.model.empty() && models.at(e.model).data)) return false;
        const auto found = materials.find(e.sprite);
        return found != materials.end() && found->second.slot != kCheckerboardSlot;
    }

    // A brush entity sits at the centre of its meshed solids, unless it carries an `origin` key.
    void placeBrush(EntityState& state) const {
        if (!state.brush || state.block->find("origin")) return;
        AABB bounds;
        bool any = false;
        for (const NodeId id : state.brushSolids) {
            const auto it = solids.find(id);
            if (it == solids.end() || !it->second.bounded) continue;
            bounds.expand(it->second.bounds.min);
            bounds.expand(it->second.bounds.max);
            any = true;
        }
        if (!any) return;
        state.position = state.pose.origin = (bounds.min + bounds.max) * 0.5;
    }

    void noteBrushSolid(NodeId solid) {
        if (const auto it = brushOwner.find(solid); it != brushOwner.end()) brushDirty.insert(it->second);
    }

    // Drops an entity's mirror state and marks the names it carried or watched as changed.
    void eraseEntity(NodeId id) {
        lightsDirty = true;
        const auto found = entities.find(id);
        if (found == entities.end()) return;
        const EntityState& state = found->second;
        for (const std::string& name : state.watching) {
            if (const auto it = watchers.find(name); it != watchers.end()) {
                it->second.erase(id);
                if (it->second.empty()) watchers.erase(it);
            }
        }
        if (!state.nameKey.empty()) {
            touchedNames.insert(state.nameKey);
            if (const auto it = byName.find(state.nameKey); it != byName.end()) {
                it->second.erase(id);
                if (it->second.empty()) byName.erase(it);
            }
        }
        helperDirty.erase(id);
        brushDirty.erase(id);
        for (const NodeId solid : state.brushSolids) {
            if (const auto it = brushOwner.find(solid); it != brushOwner.end() && it->second == id) brushOwner.erase(it);
        }
        entities.erase(found);
    }

    void rebuildLights() {
        if (!lightsDirty) return;
        lightsDirty = false;
        lights.clear();
        std::map<NodeId, kv1::Node> blocks;
        std::map<std::string, DVec3> targets;
        const auto transform = [&](NodeId id) {
            const auto it = effectivePreview.find(id);
            return it == effectivePreview.end() ? DMat4(1) : it->second;
        };
        for (const auto& [id, entity] : entities) {
            auto block = *entity.block;
            if (const auto it = helperPreview.find(id); it != helperPreview.end())
                for (const auto& [key, value] : it->second) {
                    if (auto* entry = block.find(key)) *entry = kv1::Node::value(key, value);
                    else block.append(kv1::Node::value(key, value)).value();
                }
            if (const auto* name = block.find("targetname")) {
                const auto key = lower(name->decodedValue().value_or(""));
                // Duplicate names resolve to the first visible entity in stable NodeId order.
                if (!key.empty()) targets.try_emplace(key, DVec3(transform(id) * DVec4(document::entityPose(block).origin, 1)));
            }
            blocks.emplace(id, std::move(block));
        }
        for (const auto& [id, block] : blocks) {
            const auto matrix = transform(id);
            std::optional<DVec3> target;
            if (const auto* name = block.find("target")) {
                if (const auto it = targets.find(lower(name->decodedValue().value_or(""))); it != targets.end())
                    target = DVec3(glm::inverse(matrix) * DVec4(it->second, 1));
            }
            const auto* cls = entities.at(id).cls;
            auto built = cls ? lighting::lightsFromEntity(block, *cls, target) : lighting::lightsFromEntity(block, target);
            for (auto& light : built) {
                light.origin = DVec3(matrix * DVec4(light.origin, 1));
                if (glm::length(light.direction) > 0) light.direction = glm::normalize(DMat3(matrix) * light.direction);
                lights.push_back(std::move(light));
            }
        }
    }

    struct SkyBrush {
        kv1::Node block;
        DMat4 transform{1.0};
    };

    void collectSkyBrushes(std::vector<SkyBrush>& brushes, std::set<std::string>& skyMaterials,
                           const DMat4& transform = DMat4(1)) const {
        for (const auto& [name, material] : materials)
            if (isSkyMaterial(name, material.sky)) skyMaterials.insert(name);
        for (const auto& [id, solid] : solids) {
            bool occludes = true;
            for (auto node = id; node != 0;) {
                if (const auto entity = skyEntityClasses.find(node); entity != skyEntityClasses.end()) {
                    // func_detail is compiled into world geometry and blocks VRAD. Other brush
                    // entities (func_brush etc.) do not occlude by default; leave them out.
                    occludes = entity->second == "worldspawn" || entity->second == "func_detail";
                    break;
                }
                const auto parent = hierarchy.find(node);
                node = parent == hierarchy.end() ? 0 : parent->second.parent;
            }
            if (occludes && solid.block) brushes.push_back({*solid.block, transform});
        }
    }

    bool skyPending() const { return skyWanted && skyHasAmbient && skyRevision != skyRequested; }

    void pumpSky() {
        if (!skyPending() || skyBuilding ||
            std::chrono::steady_clock::now() - skyChanged < std::chrono::milliseconds(500)) return;
        // Material flags can arrive after the geometry; don't build an immediately obsolete grid.
        if (std::ranges::any_of(materials, [](const auto& entry) { return entry.second.loading; })) return;
        std::vector<SkyBrush> brushes;
        std::set<std::string> skyMaterials;
        collectSkyBrushes(brushes, skyMaterials);
        for (const auto& content : instances) {
            if (!content.keys || instanceVisibility == document::InstanceVisibility::Hidden) continue;
            const auto& child = *instanceMirrors.at(content.keys)->impl_;
            if (std::ranges::any_of(child.materials, [](const auto& entry) { return entry.second.loading; })) return;
            child.collectSkyBrushes(brushes, skyMaterials, content.transform);
        }
        const auto revision = skyRequested;
        skyBuilding = true;
        // Only immutable values cross the worker boundary; no document handles or mirror pointers.
        skyJob = jobs.submit(Priority::Low,
            [brushes = std::move(brushes), skyMaterials = std::move(skyMaterials)](CancelToken token) {
                std::vector<lighting::SkyTriangle> triangles;
                AABB bounds;
                for (const auto& brush : brushes) {
                    if (token.cancelled()) return std::shared_ptr<const lighting::SkyVisibilityGrid>{};
                    const auto geometry = geometry::solidGeometry(brush.block);
                    if (!geometry) continue;
                    const auto& poly = geometry->poly;
                    const auto point = [&](std::size_t i) { return DVec3(brush.transform * DVec4(poly.vertices[i], 1)); };
                    for (const auto& vertex : poly.vertices) bounds.expand(DVec3(brush.transform * DVec4(vertex, 1)));
                    for (const auto& face : poly.faces) {
                        const auto name = lower(face.material);
                        const bool sky = isSkyMaterial(name, skyMaterials.contains(name));
                        if (!sky && !blocksSky(name)) continue;
                        for (const auto& tri : poly.triangulate(face)) {
                            // Kernel loops point out of solid, into air. Preserve that winding even
                            // when an instance placement reflects its geometry.
                            auto a = point(tri[0]), b = point(tri[1]), c = point(tri[2]);
                            if (glm::determinant(DMat3(brush.transform)) < 0) std::swap(b, c);
                            triangles.push_back({a, b, c, sky});
                        }
                    }
                }
                // An empty/unsealed editor map sees open sky; still use a grid, never ungated fill.
                if (bounds.empty()) bounds = {DVec3(-64), DVec3(64)};
                lighting::SkyVisibilityOptions options;
                options.cancelled = [&token] { return token.cancelled(); };
                auto grid = lighting::buildSkyVisibility(triangles, bounds, options);
                if (token.cancelled()) return std::shared_ptr<const lighting::SkyVisibilityGrid>{};
                return std::shared_ptr<const lighting::SkyVisibilityGrid>(
                    std::make_shared<lighting::SkyVisibilityGrid>(std::move(grid)));
            },
            [this, revision](std::shared_ptr<const lighting::SkyVisibilityGrid> grid) {
                skyBuilding = false;
                if (revision != skyRequested || !grid) return; // An edit overtook this build.
                skyGrid = std::move(grid);
                skyRevision = revision;
                redraw = true;
            });
    }

    void evaluateHelpers(NodeId id) {
        const auto found = entities.find(id);
        if (found == entities.end()) return;
        EntityState& state = found->second;
        for (const std::string& name : state.watching) {
            if (const auto it = watchers.find(name); it != watchers.end()) {
                it->second.erase(id);
                if (it->second.empty()) watchers.erase(it);
            }
        }
        state.watching.clear();
        state.helpers = {};
        if (!state.cls || state.cls->helpers.empty() || !state.block) return;
        auto block = document::entityHelperValues(*state.block, *state.cls);
        if (const auto preview = helperPreview.find(id); preview != helperPreview.end())
            for (const auto& [key, value] : preview->second) {
                if (auto* entry = block.find(key)) *entry = kv1::Node::value(key, value);
                else block.append(kv1::Node::value(key, value)).value();
            }
        HelperContext context{block, *state.cls, state.pose, state.colour, {}};
        context.peersNamed = [&](std::string_view name) {
            std::vector<HelperPeer> peers;
            const std::string key = lower(name);
            if (key.empty()) return peers;
            state.watching.insert(key);
            watchers[key].insert(id);
            if (const auto it = byName.find(key); it != byName.end()) {
                for (const NodeId peer : it->second) {
                    if (peer != id) peers.push_back({peer, entities.at(peer).position});
                }
            }
            return peers;
        };
        if (document::hasEntityHelper(*state.cls, "lightprop") &&
            (document::hasEntityHelper(*state.cls, "lightcone") || block.find("pitch"))) {
            std::optional<DVec3> target;
            if (const auto* name = block.find("target")) {
                const auto peers = context.peersNamed(name->decodedValue().value_or(""));
                if (!peers.empty()) target = peers.front().position;
            }
            state.pose = document::spotlightPose(block, document::entityPose(block), target);
            state.transform = document::entityTransform(state.pose);
            state.basis = state.pose.basis;
            entitiesDirty = true;
            context.pose = state.pose;
        }
        state.helpers = buildHelperShapes(context);
    }

    // Re-evaluates the entities the change touched plus any helper that looked up a changed name.
    void refreshHelpers() {
        // Move brush entities first, so helpers that look them up see the new position.
        for (const NodeId id : std::exchange(brushDirty, {})) {
            const auto found = entities.find(id);
            if (found == entities.end()) continue;
            const DVec3 before = found->second.position;
            placeBrush(found->second);
            if (found->second.position == before) continue;
            helperDirty.insert(id);
            if (!found->second.nameKey.empty()) touchedNames.insert(found->second.nameKey);
        }
        std::set<NodeId> todo = std::move(helperDirty);
        helperDirty.clear();
        for (const std::string& name : touchedNames) {
            if (const auto it = watchers.find(name); it != watchers.end()) todo.insert(it->second.begin(), it->second.end());
        }
        touchedNames.clear();
        for (const NodeId id : todo) evaluateHelpers(id);
        if (!todo.empty()) redraw = true;
    }

    void rebuildEntityBuffer() {
        entitiesDirty = false;
        if (entityBuffer) garbage.push_back(std::move(entityBuffer));
        entityBuffer = Buffer();
        entityCount = 0;
        if (spriteBuffer) garbage.push_back(std::move(spriteBuffer));
        spriteBuffer = Buffer();
        spriteCount = 0;
        collectGarbage();
        if (entities.empty()) return;
        AABB bounds;
        for (const auto& [id, e] : entities) if (!e.brush) bounds.expand(e.position);
        entityOrigin = (bounds.min + bounds.max) * 0.5;
        std::vector<BoxInstance> boxes;
        std::vector<SpriteInstance> sprites;
        for (const auto& [id, e] : entities) {
            if (e.brush || (!e.model.empty() && models.at(e.model).data)) continue;
            const DVec3 origin = e.position - entityOrigin;
            if (drawsSprite(e)) {
                sprites.push_back({{static_cast<float>(origin.x), static_cast<float>(origin.y), static_cast<float>(origin.z)},
                                   kSpriteHalfSize, {1.0f, 1.0f, 1.0f, 1.0f},
                                   materials.at(e.sprite).slot | (iconNearest ? 0x80000000u : 0u), {}});
                continue;
            }
            BoxInstance box{{static_cast<float>(e.mins.x), static_cast<float>(e.mins.y), static_cast<float>(e.mins.z)},
                            {static_cast<float>(e.maxs.x), static_cast<float>(e.maxs.y), static_cast<float>(e.maxs.z)},
                            {e.colour.x, e.colour.y, e.colour.z, e.colour.w},
                            {static_cast<float>(origin.x), static_cast<float>(origin.y), static_cast<float>(origin.z)}};
            for (int column = 0; column < 3; ++column) {
                for (int row = 0; row < 3; ++row)
                    box.basis[column][row] = static_cast<float>(e.basis[column][row]);
            }
            boxes.push_back(box);
        }
        if (!sprites.empty()) {
            if (auto buffer = upload(sprites, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
                spriteBuffer = std::move(*buffer);
                spriteCount = static_cast<std::uint32_t>(sprites.size());
            } else {
                log::error("render mirror: entity sprite upload failed");
            }
        }
        if (boxes.empty()) return;
        auto buffer = upload(boxes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!buffer) {
            log::error("render mirror: entity upload failed");
            return;
        }
        entityBuffer = std::move(*buffer);
        entityCount = static_cast<std::uint32_t>(boxes.size());
    }

    void pumpModels() {
        if (!modelCache) return;
        for (auto& [name, mesh] : models) {
            if (mesh.data) continue;
            auto data = modelCache->request(mesh.path);
            if (data == mesh.data) continue;
            for (Buffer* buffer : {&mesh.vertices, &mesh.indices, &mesh.edges})
                if (*buffer) garbage.push_back(std::move(*buffer));
            mesh.data.reset();
            mesh.materials.clear();
            entitiesDirty = redraw = true;
            if (!data) continue;
            std::vector<assets::ModelVertex> posed;
            if (!mesh.sequence.empty()) {
                const auto found = std::ranges::find_if(data->sequences, [&](const assets::ModelSequence& sequence) {
                    return lower(sequence.name) == mesh.sequence;
                });
                // Unknown or unsupported sequences keep the static pose, as the inspector does.
                if (found != data->sequences.end())
                    posed = assets::animateModel(*data, static_cast<int>(found - data->sequences.begin()), 0.0);
            }
            std::vector<BrushVertex> vertices;
            for (const auto& v : posed.empty() ? data->vertices : posed)
                vertices.push_back({{v.position.x, v.position.y, v.position.z},
                                    {v.normal.x, v.normal.y, v.normal.z}, {v.uv.x, v.uv.y}, 0});
            std::vector<std::uint32_t> edges;
            for (std::size_t i = 0; i + 2 < data->indices.size(); i += 3) {
                const auto a = data->indices[i], b = data->indices[i + 1], c = data->indices[i + 2];
                edges.insert(edges.end(), {a, b, b, c, c, a});
            }
            auto vb = upload(vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            auto ib = upload(data->indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            auto eb = upload(edges, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            if (!vb || !ib || !eb) { log::error("render mirror: model upload failed: {}", name); continue; }
            mesh.vertices = std::move(*vb);
            mesh.indices = std::move(*ib);
            mesh.edges = std::move(*eb);
            mesh.data = std::move(data);
            if (errorSlot == kCheckerboardSlot && mesh.data == modelCache->errorModel()) {
                const std::array<std::byte, 16> pixels = {
                    std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255},
                    std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255},
                    std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255},
                    std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
                if (auto slot = heap.upload({2, 2, VK_FORMAT_R8G8B8A8_UNORM}, pixels)) {
                    errorSlot = *slot;
                    uploaded = true;
                }
            }
            for (std::int32_t i = 0; i < static_cast<std::int32_t>(mesh.data->materialNames.size()); ++i) {
                auto material = assets::resolveModelMaterial(*mesh.data, i, [&](const std::string& path) {
                    return modelMaterialExists ? modelMaterialExists(path) : true;
                });
                if (!material.empty()) resolveMaterial(material);
                mesh.materials.emplace(i, std::move(material));
            }
        }
    }

    // --- Change handling -------------------------------------------------------------------

    void touchSolid(const Document& doc, Handle<document::Solid> handle, std::set<NodeId>& dirty) {
        const document::Solid* solid = doc.get(handle);
        if (!solid || !solid->block) return;
        solidIds[handle] = solid->id;
        SolidState& state = solids[solid->id];
        state.block = solid->block;
        state.hidden = hidden(doc, *solid);
        dirty.insert(solid->id);
    }

    void handleNode(const Document& doc, const NodeHandle& handle, std::set<NodeId>& dirty) {
        if (const auto* s = std::get_if<Handle<document::Solid>>(&handle)) touchSolid(doc, *s, dirty);
        else if (const auto* e = std::get_if<Handle<document::Entity>>(&handle)) updateEntity(doc, *e);
    }

    void apply(const Document& doc, const document::ChangeSet& changes) {
        if (!changes.created.empty() || !changes.modified.empty() || !changes.destroyed.empty()) invalidateSky();
        // Cache IDs only: setPreview/buildDrawList never consult or mutate the document.
        const auto remember = [&](const NodeHandle& handle) {
            if (const auto* node = doc.get(handle)) {
                hierarchy[node->id] = {node->parent, node->children};
                if (std::holds_alternative<Handle<document::Entity>>(handle) && node->block) {
                    const auto* cls = node->block->find("classname");
                    skyEntityClasses[node->id] = cls ? lower(cls->decodedValue().value_or("")) : "";
                }
                hierarchyIds[handle] = node->id;
            }
        };
        for (const auto& handle : changes.created) remember(handle);
        for (const auto& modified : changes.modified) remember(modified.handle);
        // Destroyed handles no longer resolve. Retain their IDs so deletion stays proportional
        // to the ChangeSet, including containers that do not have render state of their own.
        for (const auto& handle : changes.destroyed) {
            if (const auto found = hierarchyIds.find(handle); found != hierarchyIds.end()) {
                skyEntityClasses.erase(found->second);
                hierarchy.erase(found->second);
                hierarchyIds.erase(found);
            }
        }
        previewDirty = true;
        std::set<NodeId> dirty;
        for (const NodeHandle& h : changes.created) handleNode(doc, h, dirty);
        for (const document::Modified& m : changes.modified) {
            handleNode(doc, m.handle, dirty);
            if ((static_cast<unsigned>(m.flags) & static_cast<unsigned>(document::Dirty::Hierarchy)) == 0) continue;
            // Moving a container into/out of hidden{} changes visibility of all descendants.
            const auto refresh = [&](const auto& self, NodeId id) -> void {
                const auto h = doc.handle(id);
                const auto* node = h ? doc.get(*h) : nullptr;
                if (!node) return;
                if (const auto* solid = std::get_if<Handle<document::Solid>>(&*h)) {
                    const auto state = solids.find(id);
                    if (state != solids.end() && state->second.hidden != hidden(doc, *node))
                        touchSolid(doc, *solid, dirty);
                } else if (const auto* entity = std::get_if<Handle<document::Entity>>(&*h)) {
                    updateEntity(doc, *entity);
                }
                for (const auto child : node->children) self(self, child);
            };
            if (const auto* node = doc.get(m.handle))
                for (const auto child : node->children) refresh(refresh, child);
        }
        for (const NodeHandle& h : changes.destroyed) {
            if (const auto* s = std::get_if<Handle<document::Solid>>(&h)) {
                if (const auto it = solidIds.find(*s); it != solidIds.end()) {
                    removeSolid(it->second);
                    solidIds.erase(it);
                }
            } else if (const auto* e = std::get_if<Handle<document::Entity>>(&h)) {
                if (const auto it = entityIds.find(*e); it != entityIds.end()) {
                    eraseEntity(it->second);
                    entityIds.erase(it);
                    entitiesDirty = true;
                    redraw = true;
                }
            }
        }
        for (const NodeId id : dirty) {
            if (solids.contains(id)) requestMesh(id);
        }
        refreshHelpers();
        if (entitiesDirty) redraw = true;
        collectGarbage();
    }

    bool pending() const {
        if (skyBuilding || skyPending()) return true;
        // Hidden contents are not pumped (see pump), so their loads never finish and must not hold frames open.
        if (instanceVisibility != document::InstanceVisibility::Hidden)
            for (const auto& [keys, mirror] : instanceMirrors) if (mirror->pending()) return true;
        if (modelCache && std::ranges::any_of(models, [&](const auto& entry) {
            return !entry.second.data && (modelCache->state(entry.first) == assets::ModelState::Loading ||
                   modelCache->state(entry.first) == assets::ModelState::Unloaded);
        })) return true;
        if (previewCursor < previewGeometry.size()) return true;
        if (std::ranges::any_of(solids, [](const auto& s) { return s.second.inFlight; })) return true;
        return std::ranges::any_of(materials, [](const auto& m) { return m.second.loading; });
    }
};

RenderMirror::RenderMirror(Context& context, TextureHeap& heap, JobPool& jobs, assets::MaterialCache& materials,
                           const assets::EntityClassTable* entityClasses, assets::ModelCache* models,
                           std::function<bool(const std::string&)> modelMaterialExists)
    : impl_(std::make_unique<Impl>(context, heap, jobs, materials, entityClasses, models, std::move(modelMaterialExists))) {}
RenderMirror::~RenderMirror() = default;

void RenderMirror::setCubemapSource(CubemapSource source) {
    Impl& m = *impl_;
    if (!m.probeCubes.empty()) {
        m.context.waitIdle();
        for (const auto& [origin, cube] : m.probeCubes) m.heap.releaseCube(cube);
        m.probeCubes.clear();
        m.heap.collectGarbage();
    }
    m.cubemapSource = std::move(source);
    m.redraw = true;
}

void RenderMirror::rebuild(const Document& document) {
    impl_->loadDefaultCube();
    refreshModels();
    document::ChangeSet all;
    const auto walk = [&](const auto& self, NodeId id) -> void {
        const auto handle = document.handle(id);
        if (!handle) return;
        all.created.push_back(*handle);
        if (const document::Node* node = document.get(*handle)) {
            for (const NodeId child : node->children) self(self, child);
        }
    };
    for (const NodeId root : document.roots()) walk(walk, root);
    impl_->apply(document, all);
}

void RenderMirror::refreshModels() {
    auto& m = *impl_;
    for (auto& [keys, mirror] : m.instanceMirrors) mirror->refreshModels();
    std::set<std::string> oldMaterials;
    for (auto& [name, mesh] : m.models) {
        for (const auto& [index, material] : mesh.materials) oldMaterials.insert(material);
        for (Buffer* buffer : {&mesh.vertices, &mesh.indices, &mesh.edges})
            if (*buffer) m.garbage.push_back(std::move(*buffer));
        mesh.data.reset();
        mesh.materials.clear();
        if (m.modelCache) m.modelCache->request(mesh.path);
    }
    for (const auto& name : oldMaterials) {
        const auto found = m.materials.find(name);
        if (found == m.materials.end() || !found->second.users.empty()) continue;
        const bool previewUses = std::ranges::any_of(m.previewGeometry, [&](const auto& state) {
            return std::ranges::any_of(state.mesh.batches, [&](const auto& batch) { return batch.material == name; });
        });
        if (previewUses) continue;
        m.context.waitIdle();
        m.releaseTextures(found->second);
        m.materials.erase(found);
    }
    if (m.errorSlot != kCheckerboardSlot) {
        m.context.waitIdle();
        m.heap.release(m.errorSlot);
        m.errorSlot = kCheckerboardSlot;
    }
    m.heap.collectGarbage();
    m.entitiesDirty = m.redraw = true;
}

void RenderMirror::apply(const Document& document, const document::ChangeSet& changes) {
    impl_->apply(document, changes);
}

void RenderMirror::replaceSnapshot(const Document& before, const Document& after) {
    const auto nodes = [](const Document& doc) {
        std::map<NodeId, NodeHandle> result;
        const auto walk = [&](const auto& self, NodeId id) -> void {
            result.emplace(id, *doc.handle(id));
            for (auto child : doc.get(*doc.handle(id))->children) self(self, child);
        };
        for (auto root : doc.roots()) walk(walk, root);
        return result;
    };
    const auto oldNodes = nodes(before), newNodes = nodes(after);
    const auto bytes = [](const kv1::Node& block) {
        kv1::Document doc;
        doc.root().append(block).value();
        return kv1::write(doc).value();
    };
    document::ChangeSet removed, changed;
    for (const auto& [id, handle] : oldNodes) {
        const auto it = newNodes.find(id);
        if (it == newNodes.end() || it->second != handle) removed.destroyed.push_back(handle);
    }
    // Retire old handle mappings before introducing possibly reused template-local IDs.
    impl_->apply(after, removed);
    for (const auto& [id, handle] : newNodes) {
        const auto it = oldNodes.find(id);
        if (it == oldNodes.end() || it->second != handle) { changed.created.push_back(handle); continue; }
        const auto* a = before.get(it->second);
        const auto* b = after.get(handle);
        auto flags = document::Dirty::None;
        const bool different = bytes(*a->block) != bytes(*b->block);
        if (different) flags = document::Dirty::Geometry | document::Dirty::Material | document::Dirty::KeyValues;
        if (a->parent != b->parent) flags = flags | document::Dirty::Visibility;
        if (different || a->parent != b->parent || a->children != b->children) changed.modified.push_back({handle, flags});
    }
    impl_->apply(after, changed);
}

void RenderMirror::setInstanceContents(std::span<const document::InstanceContent> contents,
                                      document::InstanceVisibility visibility) {
    auto& m = *impl_;
    const bool same = contents.size() == m.instances.size() && std::ranges::equal(contents, m.instances,
        [](const auto& a, const auto& b) { return a.instance == b.instance && a.keys == b.keys && a.transform == b.transform; });
    if (same && m.instanceVisibility == visibility) return;
    m.invalidateSky();
    m.skyInstanceRevisions.clear();
    m.instanceVisibility = visibility;
    const auto previous = std::move(m.instances);
    m.instances.assign(contents.begin(), contents.end());
    std::set<std::shared_ptr<const Document>> used;
    for (const auto& content : contents) {
        if (!content.keys) continue;
        used.insert(content.keys);
        if (m.instanceMirrors.contains(content.keys)) continue;
        const auto old = std::ranges::find_if(previous, [&](const auto& item) {
            return item.path == content.path && item.fixup.name == content.fixup.name &&
                item.fixup.style == content.fixup.style && item.fixup.variables == content.fixup.variables &&
                item.keys != content.keys && m.instanceMirrors.contains(item.keys);
        });
        if (old != previous.end()) {
            auto mirror = std::move(m.instanceMirrors.at(old->keys));
            m.instanceMirrors.erase(old->keys);
            mirror->replaceSnapshot(*old->keys, *content.keys);
            m.instanceMirrors.emplace(content.keys, std::move(mirror));
            continue;
        }
        auto mirror = std::make_unique<RenderMirror>(m.context, m.heap, m.jobs, m.cache, m.classes,
                                                    m.modelCache, m.modelMaterialExists);
        mirror->rebuild(*content.keys);
        m.instanceMirrors.emplace(content.keys, std::move(mirror));
    }
    std::erase_if(m.instanceMirrors, [&](const auto& item) { return !used.contains(item.first); });
    m.redraw = true;
}

bool RenderMirror::pump() {
    // Demand spans all views drawn between ticks: a later Flat/ortho view must not cancel Lit.
    impl_->skyWanted = std::exchange(impl_->skyUsedSincePump, false);
    impl_->jobs.drainCompletions();
    impl_->pumpModels();
    impl_->pumpMaterials();
    impl_->meshPreviews();
    impl_->collectGarbage();
    // Hidden contents are never drawn, so their dirty flags would never clear.
    if (impl_->instanceVisibility != document::InstanceVisibility::Hidden)
        for (auto& [keys, mirror] : impl_->instanceMirrors) {
            if (mirror->pump()) impl_->redraw = true;
            auto& revision = impl_->skyInstanceRevisions[keys];
            if (revision != mirror->impl_->skyRequested) {
                revision = mirror->impl_->skyRequested;
                impl_->invalidateSky();
            }
        }
    impl_->pumpSky();
    return impl_->redraw;
}

bool RenderMirror::settle(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const bool skyWanted = impl_->skyWanted;
    for (;;) {
        // Headless callers explicitly wait for the last requested view without drawing each tick.
        impl_->skyUsedSincePump |= skyWanted;
        pump();
        if (!impl_->pending()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool RenderMirror::needsRedraw() const {
    if (impl_->redraw || impl_->pending()) return true;
    if (impl_->instanceVisibility == document::InstanceVisibility::Hidden) return false;
    return std::ranges::any_of(impl_->instanceMirrors, [](const auto& item) { return item.second->needsRedraw(); });
}
bool RenderMirror::pending() const { return impl_->pending(); }
SkyVisibilityState RenderMirror::skyVisibilityState() const {
    return {impl_->skyRevision, impl_->skyRequested, impl_->skyBuilding};
}

void RenderMirror::setHighlight(const Highlight& highlight) {
    if (impl_->highlight == highlight) return;
    impl_->highlight = highlight;
    impl_->selected = {highlight.selected.begin(), highlight.selected.end()};
    impl_->selectedSides = {highlight.selectedSides.begin(), highlight.selectedSides.end()};
    impl_->redraw = true;
}

void RenderMirror::setPreview(const PreviewTransforms& preview) {
    if (impl_->preview == preview && impl_->previewGeometry.empty() && impl_->helperPreview.empty()) return;
    if (impl_->preview != preview || !impl_->helperPreview.empty()) impl_->lightsDirty = true;
    impl_->clearGeometry();
    for (const auto& [id, keys] : impl_->helperPreview) impl_->helperDirty.insert(id);
    impl_->helperPreview.clear();
    impl_->refreshHelpers();
    impl_->preview = preview;
    impl_->previewDirty = true;
    impl_->redraw = true;
}

void RenderMirror::setPreview(const PreviewTransforms& transforms, const PreviewGeometry& geometry, std::uint64_t revision) {
    if (impl_->previewRevision == revision) return;
    if (impl_->preview != transforms || impl_->helperPreview != geometry.entityKeys) impl_->lightsDirty = true;
    impl_->clearGeometry();
    impl_->previewRevision = revision;
    for (const auto& [id, keys] : impl_->helperPreview) impl_->helperDirty.insert(id);
    impl_->helperPreview = geometry.entityKeys;
    for (const auto& [id, keys] : impl_->helperPreview) impl_->helperDirty.insert(id);
    impl_->refreshHelpers();
    impl_->preview = transforms;
    impl_->previewDirty = true;
    impl_->redraw = true;
    for (const auto& [id, poly] : geometry.replacements) {
        Impl::PreviewState state;
        state.node = id;
        state.shape.poly = poly;
        state.shape.opacity = 1.0f;
        impl_->previewGeometry.push_back(std::move(state));
    }
    for (const auto& ghost : geometry.ghosts) {
        Impl::PreviewState state;
        state.shape = ghost;
        impl_->previewGeometry.push_back(std::move(state));
    }
    // Meshing waits for pump(), so several revisions between ticks share one budget and one submit.
}

void RenderMirror::setPreviewBudget(std::chrono::nanoseconds budget) {
    impl_->previewBudget = std::max(budget, std::chrono::nanoseconds::zero());
}
std::size_t RenderMirror::pendingPreviewCount() const {
    return impl_->previewGeometry.size() - impl_->previewCursor;
}
void RenderMirror::clearPreview() { setPreview(PreviewTransforms{}); }

DrawList RenderMirror::buildDrawList(const MirrorViewOptions& options) {
    const bool textured = options.mode == DisplayMode::Textured || options.mode == DisplayMode::Lit;
    impl_->refreshHelpers(); // Brush entities move when their solids finish meshing, between apply() calls.
    Impl& m = *impl_;
    m.redraw = false;
    if (m.iconNearest != options.iconNearest) {
        m.iconNearest = options.iconNearest;
        m.entitiesDirty = true;
    }
    if (m.entitiesDirty) m.rebuildEntityBuffer();
    DrawList list;
    list.mode = options.mode;
    list.view = options.view;
    list.grid = options.grid;
    list.edgeOverlay = options.edgeOverlay;
    list.edgeColour = options.edgeColour;
    list.occludedAlpha = m.highlight.occludedAlpha;
    m.resolvePreview();
    m.rebuildLights();
    list.lights = m.lights;
    if (options.mode == DisplayMode::Lit) {
        for (const auto& [id, entity] : m.entities) {
            if (!entity.cubemap) continue;
            const auto found = m.effectivePreview.find(id);
            const DVec3 position = found == m.effectivePreview.end() ? entity.position :
                DVec3(found->second * DVec4(entity.position, 1));
            list.cubemaps.push_back({position, kDefaultCubeSlot});
        }
    }
    const auto previewIndex = [&](NodeId id, DVec3 origin) -> std::uint32_t {
        const auto found = m.effectivePreview.find(id);
        if (found == m.effectivePreview.end()) return 0;
        list.previews.push_back({found->second, origin});
        return static_cast<std::uint32_t>(list.previews.size());
    };
    std::set<NodeId> replaced;
    for (const auto& state : m.previewGeometry) if (state.node) replaced.insert(state.node);
    // A selected sidelist entity (info_overlay) highlights the faces it lists, as if they were picked.
    std::set<document::SideId> sides = m.selectedSides;
    for (const auto& [id, entity] : m.entities) {
        if (entity.helpers.sideIds.empty()) continue;
        const bool picked = m.selected.contains(id) ||
                            std::ranges::any_of(entity.brushSolids, [&](NodeId solid) { return m.selected.contains(solid); });
        if (!picked) continue;
        for (const int side : entity.helpers.sideIds) sides.insert(static_cast<document::SideId>(side));
    }
    for (const auto& [id, solid] : m.solids) {
        if (replaced.contains(id)) continue;
        if (solid.hidden || !solid.meshed) continue;
        const auto preview = previewIndex(id, solid.origin);
        for (const GpuBatch& batch : solid.batches) {
            MeshDraw draw;
            draw.vertices = &batch.vertices;
            draw.indices = &batch.indices;
            draw.indexCount = batch.indexCount;
            draw.edgeIndices = &batch.edges;
            draw.edgeIndexCount = batch.edgeCount;
            draw.origin = solid.origin;
            draw.previewIndex = preview;
            const auto material = m.materials.find(batch.material);
            if (material != m.materials.end()) {
                draw.texture = material->second.slot;
            }
            // Wireframe keeps every edge: 2D views would otherwise lose the nodraw faces' outline.
            const MaterialState* state = material != m.materials.end() ? &material->second : nullptr;
            const bool hideFill = options.mode != DisplayMode::Wireframe &&
                                  ((!options.showNodraw && isNodraw(batch.material, state)) ||
                                   (!options.showToolFaces && isToolFace(batch.material, state)));
            if (textured && material != m.materials.end()) draw.alpha = material->second.alpha;
            switch (options.mode) {
            case DisplayMode::Lit:
            case DisplayMode::Textured: draw.colour = Vec4(1.0f); break;
            case DisplayMode::Flat: draw.colour = flatColour(batch.material); break;
            case DisplayMode::Wireframe: draw.colour = options.wireColour; break;
            }
            const bool selected = m.selected.contains(id);
            const Vec4 tint = selected ? m.highlight.selectTint
                                      : m.highlight.hover == id ? m.highlight.hoverTint : Vec4(0.0f);
            draw.highlight = tint;
            draw.outlineOnlyWire = selected;
            const bool faceMode = !selected && std::ranges::any_of(batch.faces, [&](const FaceRange& face) {
                return sides.contains(face.side);
            });
            if (faceMode) {
                // Split existing ranges so a face tint does not bleed into its material siblings.
                for (const FaceRange& face : batch.faces) {
                    MeshDraw part = draw;
                    part.firstIndex = face.firstIndex;
                    part.indexCount = face.indexCount;
                    part.firstEdgeIndex = face.firstEdgeIndex;
                    part.edgeIndexCount = face.edgeIndexCount;
                    if (sides.contains(face.side)) {
                        part.highlight = m.highlight.selectTint;
                        part.outlineOnlyWire = true;
                        MeshDraw outline = part;
                        outline.colour = m.highlight.outlineColour;
                        outline.highlight = Vec4(0.0f);
                        list.outlines.push_back(outline);
                    }
                    if (!hideFill) list.meshes.push_back(part);
                }
            } else if (!hideFill) {
                list.meshes.push_back(draw);
            }
            if (selected) {
                draw.colour = m.highlight.outlineColour;
                draw.highlight = Vec4(0.0f);
                list.outlines.push_back(draw);
            }
        }
    }
    for (const auto& state : m.previewGeometry) {
        if (state.node) {
            const auto original = m.solids.find(state.node);
            if (original == m.solids.end() || original->second.hidden) continue;
        }
        const auto matrix = m.effectivePreview.find(state.node);
        const auto point = [&](DVec3 p) {
            return matrix == m.effectivePreview.end() ? p : DVec3(matrix->second * DVec4(p, 1.0));
        };
        if (!state.mesh.meshed) {
            const auto& poly = state.shape.poly;
            for (const auto& face : poly.faces) {
                for (std::size_t i = 0; i < face.vertices.size(); ++i) {
                    const auto a = face.vertices[i], b = face.vertices[(i + 1) % face.vertices.size()];
                    if (a < poly.vertices.size() && b < poly.vertices.size())
                        list.worldSegments.push_back({point(poly.vertices[a]), point(poly.vertices[b]), state.shape.outline, 1.0f});
                }
            }
            continue;
        }
        const auto preview = previewIndex(state.node, state.mesh.origin);
        for (const auto& batch : state.mesh.batches) {
            MeshDraw draw;
            draw.vertices = &batch.vertices;
            draw.indices = &batch.indices;
            draw.indexCount = batch.indexCount;
            draw.edgeIndices = &batch.edges;
            draw.edgeIndexCount = batch.edgeCount;
            draw.origin = state.mesh.origin;
            draw.previewIndex = preview;
            const MaterialState& material = m.materials.at(batch.material);
            draw.texture = material.slot;
            draw.colour = options.mode == DisplayMode::Flat ? flatColour(batch.material) : Vec4(1.0f);
            draw.colour.a = state.shape.opacity;
            if (state.node) {
                if (textured) draw.alpha = material.alpha; // Ghosts stay uniformly translucent.
                if (m.selected.contains(state.node)) draw.highlight = m.highlight.selectTint;
                else if (m.highlight.hover == state.node) draw.highlight = m.highlight.hoverTint;
                const bool hideFill = options.mode != DisplayMode::Wireframe &&
                                      ((!options.showNodraw && isNodraw(batch.material, &material)) ||
                                       (!options.showToolFaces && isToolFace(batch.material, &material)));
                if (!hideFill) list.meshes.push_back(draw);
            } else {
                list.ghosts.push_back(draw);
            }
            if (!state.node || m.selected.contains(state.node)) {
                draw.colour = state.node ? m.highlight.outlineColour : state.shape.outline;
                list.outlines.push_back(draw);
            }
        }
    }
    if (options.showEntities && options.showEntityHelpers && m.entityBuffer && m.entityCount > 0) {
        std::uint32_t instance = 0;
        for (const auto& [id, entity] : m.entities) {
            if (entity.brush || (!entity.model.empty() && m.models.at(entity.model).data) || m.drawsSprite(entity)) continue;
            BoxDraw draw{&m.entityBuffer, 1, m.entityOrigin, options.mode != DisplayMode::Wireframe};
            draw.firstInstance = instance++;
            draw.previewIndex = previewIndex(id, m.entityOrigin);
            const bool selected = m.selected.contains(id);
            draw.tint = selected ? m.highlight.selectTint
                                 : m.highlight.hover == id ? m.highlight.hoverTint : Vec4(0.0f);
            draw.outlineOnlyWire = selected;
            list.boxes.push_back(draw);
            if (selected) {
                draw.solid = false;
                draw.tint = m.highlight.outlineColour;
                list.boxOutlines.push_back(draw);
            }
        }
    }
    if (options.showEntities && options.showEntityHelpers && m.spriteBuffer && m.spriteCount > 0) {
        std::uint32_t instance = 0;
        for (const auto& [id, entity] : m.entities) {
            if (!m.drawsSprite(entity)) continue;
            SpriteDraw draw{&m.spriteBuffer, 1, m.entityOrigin};
            draw.firstInstance = instance++;
            draw.previewIndex = previewIndex(id, m.entityOrigin);
            const bool selected = m.selected.contains(id);
            draw.tint = selected ? m.highlight.selectTint
                                 : m.highlight.hover == id ? m.highlight.hoverTint : Vec4(0.0f);
            list.sprites.push_back(draw);
            if (selected) {
                draw.tint = m.highlight.outlineColour;
                list.spriteOutlines.push_back(draw);
            }
        }
    }
    if (options.showEntities && options.showEntityHelpers) {
        // A brush entity is carried by its solids, so a preview on either counts for it.
        const auto previewOf = [&](NodeId id) {
            auto it = m.effectivePreview.find(id);
            if (it != m.effectivePreview.end()) return it;
            if (const auto entity = m.entities.find(id); entity != m.entities.end()) {
                for (const NodeId solid : entity->second.brushSolids) {
                    it = m.effectivePreview.find(solid);
                    if (it != m.effectivePreview.end()) return it;
                }
            }
            return m.effectivePreview.end();
        };
        const auto moved = [&](NodeId id, DVec3 p) {
            const auto it = previewOf(id);
            return it == m.effectivePreview.end() ? p : DVec3(it->second * DVec4(p, 1.0));
        };
        const auto picked = [&](NodeId id, const EntityState& entity) {
            return m.selected.contains(id) || std::ranges::any_of(entity.brushSolids, [&](NodeId solid) { return m.selected.contains(solid); });
        };
        std::set<NodeId> resolvedInstances;
        for (const auto& content : m.instances) resolvedInstances.insert(content.instance);
        for (const auto& [id, entity] : m.entities) {
            const bool self = picked(id, entity);
            for (const HelperSegment& segment : entity.helpers.segments) {
                const auto peerPicked = [&](const std::optional<NodeId>& peerId) {
                    if (!peerId) return false;
                    const auto found = m.entities.find(*peerId);
                    return found != m.entities.end() ? picked(*peerId, found->second) : m.selected.contains(*peerId);
                };
                const bool peer = peerPicked(segment.peer) || peerPicked(segment.anchor);
                const bool shown = segment.visibility == HelperVisibility::Always ||
                                   (self && segment.visibility == HelperVisibility::WhenSelected) ||
                                   ((self || peer) && segment.visibility == HelperVisibility::WhenSelfOrPeerSelected);
                if (!shown) continue;
                list.worldSegments.push_back({moved(segment.anchor.value_or(id), segment.a), moved(segment.peer.value_or(id), segment.b),
                                              segment.colour, 1.0f});
            }
            for (const HelperLabel& label : entity.helpers.labels)
                if (self || label.visibility == HelperVisibility::Always) list.labels.push_back({moved(id, label.position), label.text, Vec2(0.0f), label.colour});
            if (options.showEntityNames && !entity.targetname.empty())
                list.labels.push_back({moved(id, entity.position), entity.targetname, Vec2(8.0f, -8.0f), options.wireColour});
            // The path is noise on every placed instance; show it for the one being inspected, and
            // always for an instance whose file did not resolve (it has no contents to speak for it).
            // Nested ones (a template's own func_instances) show it only while their owner is inspected.
            const bool showPath = m.drawsInstanceContents ? m.ownerInspected
                                                          : self || m.highlight.hover == id || !resolvedInstances.contains(id);
            if (!entity.instanceFile.empty() && showPath)
                list.labels.push_back({moved(id, entity.position), entity.instanceFile, Vec2(8.0f, 8.0f), options.wireColour});
        }
    }
    if (options.showEntities) {
        struct Group { MeshDraw draw; std::vector<DMat4> transforms; };
        std::vector<Group> groups;
        for (const auto& [id, entity] : m.entities) {
            if (entity.model.empty()) continue;
            const auto& mesh = m.models.at(entity.model);
            if (!mesh.data) continue;
            DMat4 transform = entity.transform;
            if (const auto it = m.effectivePreview.find(id); it != m.effectivePreview.end()) transform = it->second * transform;
            const Vec4 tint = m.selected.contains(id) ? m.highlight.selectTint
                : m.highlight.hover == id ? m.highlight.hoverTint : Vec4(0.0f);
            for (const auto& range : assets::modelDrawRanges(*mesh.data, entity.body, entity.skin)) {
                if (range.indexCount == 0) continue;
                MeshDraw draw;
                draw.vertices = &mesh.vertices;
                draw.indices = &mesh.indices;
                draw.edgeIndices = &mesh.edges;
                draw.firstIndex = range.firstIndex;
                draw.indexCount = range.indexCount;
                draw.firstEdgeIndex = range.firstIndex * 2;
                draw.edgeIndexCount = range.indexCount * 2;
                draw.edgeOverlay = options.modelEdgeOverlay;
                const auto material = mesh.materials.find(range.material);
                if (material != mesh.materials.end() && !material->second.empty()) {
                    const auto& state = m.materials.at(material->second);
                    draw.texture = state.slot;
                    if (textured) draw.alpha = state.alpha;
                    draw.colour = options.mode == DisplayMode::Flat ? flatColour(material->second) : Vec4(1.0f);
                } else {
                    draw.colour = entity.colour;
                    if (range.material < 0) draw.texture = m.errorSlot;
                }
                if (options.mode == DisplayMode::Wireframe) draw.colour = options.wireColour;
                draw.highlight = tint;
                auto group = std::ranges::find_if(groups, [&](const Group& g) {
                    const auto& d = g.draw;
                    return d.vertices == draw.vertices && d.firstIndex == draw.firstIndex &&
                        d.indexCount == draw.indexCount && d.alpha == draw.alpha && d.texture == draw.texture && d.colour == draw.colour && d.highlight == tint;
                });
                if (group == groups.end()) { groups.push_back({draw, {}}); group = std::prev(groups.end()); }
                group->transforms.push_back(transform);
            }
        }
        for (auto& group : groups) {
            group.draw.previewIndex = static_cast<std::uint32_t>(list.previews.size() + 1);
            group.draw.instanceCount = static_cast<std::uint32_t>(group.transforms.size());
            group.draw.instancedTransforms = true;
            for (const auto& transform : group.transforms) list.previews.push_back({transform, DVec3(0.0)});
            list.meshes.push_back(group.draw);
            const bool outlined = options.modelEdgeOverlay || options.mode == DisplayMode::Wireframe;
            if (outlined && group.draw.highlight == m.highlight.selectTint && !m.selected.empty()) {
                MeshDraw outline = group.draw;
                outline.colour = m.highlight.outlineColour;
                outline.highlight = Vec4(0.0f);
                list.outlines.push_back(outline);
            }
        }
    }
    m.drawnInstanceSolids = 0;
    if (m.instanceVisibility != document::InstanceVisibility::Hidden) {
        for (const auto& content : m.instances) {
            // The parent's entity state is also its visibility gate (hidden containers etc.).
            if (!content.keys || !m.entities.contains(content.instance)) continue;
            auto& mirror = *m.instanceMirrors.at(content.keys);
            Highlight highlight = m.highlight;
            highlight.selected.clear();
            highlight.selectedSides.clear();
            highlight.hover.reset();
            const bool selected = m.selected.contains(content.instance);
            const bool hovered = m.highlight.hover == content.instance;
            const Vec4 tint = selected ? m.highlight.selectTint : hovered ? m.highlight.hoverTint :
                m.instanceVisibility == document::InstanceVisibility::Tinted ? Vec4(0.3f, 1.0f, 0.4f, 0.35f) : Vec4(0.0f);
            // Selecting the owner reveals its contents' selection-dependent helpers too.
            if (selected) for (const auto& [id, hierarchy] : mirror.impl_->hierarchy) highlight.selected.push_back(id);
            mirror.setHighlight(highlight);
            mirror.impl_->drawsInstanceContents = true;
            mirror.impl_->ownerInspected = selected || hovered;
            auto child = mirror.buildDrawList(options);
            m.drawnInstanceSolids += mirror.meshedSolidCount();
            DMat4 transform = content.transform;
            if (const auto it = m.effectivePreview.find(content.instance); it != m.effectivePreview.end()) transform = it->second * transform;
            const auto point = [&](DVec3 p) { return DVec3(transform * DVec4(p, 1.0)); };
            for (auto light : child.lights) {
                light.origin = point(light.origin);
                if (glm::length(light.direction) > 0) light.direction = glm::normalize(DMat3(transform) * light.direction);
                list.lights.push_back(std::move(light));
            }
            for (auto probe : child.cubemaps) {
                probe.position = point(probe.position);
                list.cubemaps.push_back(probe);
            }
            const auto preview = [&](std::uint32_t index, DVec3 origin, std::uint32_t count = 1) {
                const auto first = static_cast<std::uint32_t>(list.previews.size() + 1);
                if (index) {
                    for (std::uint32_t i = 0; i < count; ++i) {
                        const auto& p = child.previews[index - 1 + i];
                        list.previews.push_back({transform * p.matrix, p.origin});
                    }
                } else list.previews.push_back({transform, origin});
                return first;
            };
            const auto meshes = [&](const auto& source, auto& destination, bool outline = false) {
                for (auto draw : source) {
                    draw.previewIndex = preview(draw.previewIndex, draw.origin, draw.instancedTransforms ? draw.instanceCount : 1);
                    if (!outline) draw.highlight = tint;
                    destination.push_back(draw);
                }
            };
            meshes(child.meshes, list.meshes);
            meshes(child.outlines, list.outlines, true);
            meshes(child.ghosts, list.ghosts);
            for (auto draw : child.boxes) {
                draw.previewIndex = preview(draw.previewIndex, draw.origin);
                draw.tint = tint;
                list.boxes.push_back(draw);
            }
            for (auto draw : child.boxOutlines) {
                draw.previewIndex = preview(draw.previewIndex, draw.origin);
                list.boxOutlines.push_back(draw);
            }
            for (auto draw : child.sprites) {
                draw.previewIndex = preview(draw.previewIndex, draw.origin);
                draw.tint = tint;
                list.sprites.push_back(draw);
            }
            for (auto draw : child.spriteOutlines) {
                draw.previewIndex = preview(draw.previewIndex, draw.origin);
                list.spriteOutlines.push_back(draw);
            }
            for (auto segment : child.worldSegments) {
                segment.a = point(segment.a); segment.b = point(segment.b);
                list.worldSegments.push_back(segment);
            }
            for (auto label : child.labels) {
                label.position = point(label.position);
                list.labels.push_back(std::move(label));
            }
        }
    }
    if (options.mode == DisplayMode::Lit && !m.drawsInstanceContents) {
        m.skyWanted = true;
        m.skyUsedSincePump = true;
    }
    m.skyHasAmbient = std::ranges::any_of(list.lights, [](const auto& light) {
        return light.kind == lighting::LightKind::Ambient;
    });
    m.pumpSky();
    // Only Lit samples the grid; other modes must not pay for its upload.
    if (options.mode == DisplayMode::Lit) list.skyVisibility = m.skyGrid;
    // Probe cubes are named by world origin, so only the outermost mirror resolves them.
    if (options.mode == DisplayMode::Lit && !m.drawsInstanceContents && m.cubemapSource && !list.cubemaps.empty()) {
        const auto cubes = m.heap.liveCubes();
        for (auto& probe : list.cubemaps) probe.cube = m.probeCube(probe.position);
        if (m.heap.liveCubes() != cubes) {
            if (auto flushed = m.heap.flush(); !flushed) log::error("render mirror: cubemap upload: {}", flushed.error().what());
        }
    }
    return list;
}

const MirrorStats& RenderMirror::stats() const { return impl_->stats; }
void RenderMirror::resetStats() {
    impl_->stats = {};
    for (auto& [keys, mirror] : impl_->instanceMirrors) mirror->resetStats();
}
std::size_t RenderMirror::solidCount() const { return impl_->solids.size(); }
std::size_t RenderMirror::meshedSolidCount() const {
    return static_cast<std::size_t>(std::ranges::count_if(impl_->solids, [](const auto& s) { return s.second.meshed; }));
}
std::size_t RenderMirror::entityBoxCount() const {
    return static_cast<std::size_t>(std::ranges::count_if(impl_->entities, [](const auto& e) { return !e.second.brush; }));
}
std::size_t RenderMirror::batchCount() const {
    std::size_t count = 0;
    for (const auto& [id, solid] : impl_->solids) count += solid.batches.size();
    return count;
}

std::size_t RenderMirror::meshBytes() const {
    std::size_t bytes = impl_->entityBuffer.size() + impl_->spriteBuffer.size();
    for (const auto& [id, solid] : impl_->solids)
        for (const auto& batch : solid.batches) bytes += batch.vertices.size() + batch.indices.size() + batch.edges.size();
    for (const auto& [name, model] : impl_->models) bytes += model.vertices.size() + model.indices.size() + model.edges.size();
    for (const auto& [keys, mirror] : impl_->instanceMirrors) bytes += mirror->meshBytes();
    return bytes;
}
InstanceMirrorStats RenderMirror::instanceStats() const {
    InstanceMirrorStats result;
    result.placements = impl_->instances.size();
    result.drawnSolids = impl_->drawnInstanceSolids;
    for (const auto& [keys, mirror] : impl_->instanceMirrors) {
        result.uniqueSolids += mirror->meshedSolidCount();
        result.meshBytes += mirror->meshBytes();
        result.textureBytes += mirror->textureBytes();
        result.jobsSubmitted += mirror->stats().jobsSubmitted;
        result.solidsRemeshed += mirror->stats().solidsRemeshed;
    }
    return result;
}

std::size_t RenderMirror::textureBytes() const {
    std::size_t bytes = impl_->errorSlot == kCheckerboardSlot ? 0 : 16;
    for (const auto& [name, material] : impl_->materials) bytes += material.textureBytes;
    for (const auto& [keys, mirror] : impl_->instanceMirrors) bytes += mirror->textureBytes();
    return bytes;
}

} // namespace apex::render
