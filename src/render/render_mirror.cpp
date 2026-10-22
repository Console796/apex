#include "render/render_mirror.hpp"

#include "core/log.hpp"
#include "render/solid_mesh.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
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

std::optional<VkFormat> vulkanFormat(assets::TextureFormat format) {
    using F = assets::TextureFormat;
    switch (format) {
    case F::Rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
    case F::Bc1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case F::Bc2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case F::Bc3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case F::Bc4: return VK_FORMAT_BC4_UNORM_BLOCK;
    case F::Bc5: return VK_FORMAT_BC5_UNORM_BLOCK;
    case F::Bc6Signed: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case F::Bc6Unsigned: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case F::Bc7: return VK_FORMAT_BC7_UNORM_BLOCK;
    case F::Rgba32Float: return std::nullopt;
    }
    return std::nullopt;
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
    std::vector<GpuBatch> batches;
    std::vector<std::string> materials;
};

struct EntityState {
    DVec3 position{0.0}, mins{-8.0}, maxs{8.0};
    Vec4 colour{0.9f, 0.4f, 0.9f, 1.0f};
};

struct MaterialState {
    TextureSlot slot = kCheckerboardSlot;
    TextureSize size;
    bool loading = false;
    std::set<NodeId> users;
};

} // namespace

struct RenderMirror::Impl {
    Context& context;
    TextureHeap& heap;
    JobPool& jobs;
    assets::MaterialCache& cache;
    const assets::EntityClassTable* classes;

    std::map<NodeId, SolidState> solids;
    std::map<Handle<document::Solid>, NodeId> solidIds;
    std::map<Handle<document::Entity>, NodeId> entityIds;
    std::map<NodeId, EntityState> entities;
    std::map<std::string, MaterialState, std::less<>> materials;
    TextureSize placeholderSize;

    Buffer entityBuffer;
    std::uint32_t entityCount = 0;
    DVec3 entityOrigin{0.0};
    bool entitiesDirty = false;
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

    Impl(Context& c, TextureHeap& h, JobPool& j, assets::MaterialCache& m, const assets::EntityClassTable* k)
        : context(c), heap(h), jobs(j), cache(m), classes(k) {
        const auto placeholder = cache.placeholder();
        if (placeholder && !placeholder->texture.mips.empty()) {
            placeholderSize = {std::max<int>(1, placeholder->texture.mips[0].width),
                               std::max<int>(1, placeholder->texture.mips[0].height)};
        }
    }

    ~Impl() {
        for (auto& [id, solid] : solids) solid.job.cancel();
        context.waitIdle();
        for (auto& [name, material] : materials) {
            if (material.slot != kCheckerboardSlot) heap.release(material.slot);
        }
        heap.collectGarbage();
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
        const assets::Texture& texture = material.texture;
        const auto format = vulkanFormat(texture.format);
        if (!format || texture.mips.empty()) return;
        std::vector<std::span<const std::byte>> mips;
        std::uint32_t w = texture.mips[0].width, h = texture.mips[0].height;
        if (w == 0 || h == 0) return;
        for (const assets::TextureMip& mip : texture.mips) {
            const VkDeviceSize need = mipByteSize(*format, w, h);
            if (mip.width != w || mip.height != h || mip.data.size() < need) break;
            mips.emplace_back(mip.data.data(), static_cast<std::size_t>(need)); // First frame/face.
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        if (mips.empty()) return;
        auto slot = heap.upload({texture.mips[0].width, texture.mips[0].height, *format}, mips);
        if (!slot) {
            log::warn("render mirror: texture upload for '{}' failed: {}", std::string_view(name),
                      std::string_view(slot.error().message));
            return;
        }
        if (state.slot != kCheckerboardSlot) heap.release(state.slot);
        state.slot = *slot;
        state.size = {texture.mips[0].width, texture.mips[0].height};
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
            const TextureSize before = state.size;
            if (const auto material = cache.request(name)) uploadMaterial(name, state, *material);
            if (!(before == state.size)) remesh.insert(remesh.end(), state.users.begin(), state.users.end());
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

    Result<Buffer> upload(const auto& data, VkBufferUsageFlags usage) {
        return uploadBuffer(context, heap.uploadRing(), std::as_bytes(std::span{data}), usage);
    }

    void complete(NodeId id, std::uint64_t version, Result<SolidMesh> mesh) {
        const auto it = solids.find(id);
        if (it == solids.end() || it->second.version != version) return; // Stale or removed.
        SolidState& solid = it->second;
        solid.inFlight = false;
        ++stats.solidsRemeshed;
        redraw = true;
        retire(solid);
        if (!mesh) {
            ++stats.meshFailures;
            log::warn("render mirror: solid {}: {}", static_cast<unsigned long long>(id),
                      std::string_view(mesh.error().message));
            return;
        }
        solid.origin = mesh->origin;
        for (BatchMesh& batch : mesh->batches) {
            if (batch.indices.empty()) continue;
            auto vertices = upload(batch.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            auto indices = upload(batch.indices, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            auto edges = upload(batch.edges, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            if (!vertices || !indices || !edges) {
                log::error("render mirror: mesh upload failed for solid {}", static_cast<unsigned long long>(id));
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
        const document::Entity* entity = doc.get(handle);
        if (!entity) return;
        entityIds[handle] = entity->id;
        const bool changed = entities.erase(entity->id) > 0;
        entitiesDirty = true;
        (void)changed;
        if (!entity->block || hidden(doc, *entity)) return;
        for (const NodeId child : entity->children) {
            const auto childHandle = doc.handle(child);
            if (childHandle && std::holds_alternative<Handle<document::Solid>>(*childHandle)) return; // Brush entity.
        }
        std::string classname;
        if (const kv1::Node* c = entity->block->find("classname")) {
            if (auto text = c->decodedValue()) classname = *text;
        }
        if (lower(classname) == "worldspawn") return;
        EntityState state;
        if (const kv1::Node* o = entity->block->find("origin")) {
            if (auto v = o->asVec3()) state.position = {(*v)[0], (*v)[1], (*v)[2]};
        }
        if (classes) {
            if (const assets::EntityClass* cls = classes->find(classname)) {
                if (cls->mins) state.mins = {(*cls->mins)[0], (*cls->mins)[1], (*cls->mins)[2]};
                if (cls->maxs) state.maxs = {(*cls->maxs)[0], (*cls->maxs)[1], (*cls->maxs)[2]};
                if (cls->color) {
                    state.colour = {static_cast<float>((*cls->color)[0]) / 255.0f,
                                    static_cast<float>((*cls->color)[1]) / 255.0f,
                                    static_cast<float>((*cls->color)[2]) / 255.0f, 1.0f};
                }
            }
        }
        entities[entity->id] = state;
    }

    void rebuildEntityBuffer() {
        entitiesDirty = false;
        if (entityBuffer) garbage.push_back(std::move(entityBuffer));
        entityBuffer = Buffer();
        entityCount = 0;
        collectGarbage();
        if (entities.empty()) return;
        AABB bounds;
        for (const auto& [id, e] : entities) bounds.expand(e.position);
        entityOrigin = (bounds.min + bounds.max) * 0.5;
        std::vector<BoxInstance> boxes;
        for (const auto& [id, e] : entities) {
            const DVec3 lo = e.position + e.mins - entityOrigin, hi = e.position + e.maxs - entityOrigin;
            boxes.push_back({{static_cast<float>(lo.x), static_cast<float>(lo.y), static_cast<float>(lo.z)},
                             {static_cast<float>(hi.x), static_cast<float>(hi.y), static_cast<float>(hi.z)},
                             {e.colour.x, e.colour.y, e.colour.z, e.colour.w}});
        }
        auto buffer = upload(boxes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (!buffer) {
            log::error("render mirror: entity upload failed");
            return;
        }
        entityBuffer = std::move(*buffer);
        entityCount = static_cast<std::uint32_t>(boxes.size());
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
        // Cache IDs only: setPreview/buildDrawList never consult or mutate the document.
        const auto remember = [&](const NodeHandle& handle) {
            if (const auto* node = doc.get(handle)) {
                hierarchy[node->id] = {node->parent, node->children};
                hierarchyIds[handle] = node->id;
            }
        };
        for (const auto& handle : changes.created) remember(handle);
        for (const auto& modified : changes.modified) remember(modified.handle);
        // Destroyed handles no longer resolve. Retain their IDs so deletion stays proportional
        // to the ChangeSet, including containers that do not have render state of their own.
        for (const auto& handle : changes.destroyed) {
            if (const auto found = hierarchyIds.find(handle); found != hierarchyIds.end()) {
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
                    entities.erase(it->second);
                    entityIds.erase(it);
                    entitiesDirty = true;
                    redraw = true;
                }
            }
        }
        for (const NodeId id : dirty) {
            if (solids.contains(id)) requestMesh(id);
        }
        if (entitiesDirty) redraw = true;
        collectGarbage();
    }

    bool pending() const {
        if (std::ranges::any_of(solids, [](const auto& s) { return s.second.inFlight; })) return true;
        return std::ranges::any_of(materials, [](const auto& m) { return m.second.loading; });
    }
};

RenderMirror::RenderMirror(Context& context, TextureHeap& heap, JobPool& jobs, assets::MaterialCache& materials,
                           const assets::EntityClassTable* entityClasses)
    : impl_(std::make_unique<Impl>(context, heap, jobs, materials, entityClasses)) {}
RenderMirror::~RenderMirror() = default;

void RenderMirror::rebuild(const Document& document) {
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

void RenderMirror::apply(const Document& document, const document::ChangeSet& changes) {
    impl_->apply(document, changes);
}

bool RenderMirror::pump() {
    impl_->jobs.drainCompletions();
    impl_->pumpMaterials();
    impl_->collectGarbage();
    return impl_->redraw;
}

bool RenderMirror::settle(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        pump();
        if (!impl_->pending()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool RenderMirror::needsRedraw() const { return impl_->redraw || impl_->pending(); }
bool RenderMirror::pending() const { return impl_->pending(); }

void RenderMirror::setHighlight(const Highlight& highlight) {
    if (impl_->highlight == highlight) return;
    impl_->highlight = highlight;
    impl_->selected = {highlight.selected.begin(), highlight.selected.end()};
    impl_->selectedSides = {highlight.selectedSides.begin(), highlight.selectedSides.end()};
    impl_->redraw = true;
}

void RenderMirror::setPreview(const PreviewTransforms& preview) {
    if (impl_->preview == preview) return;
    impl_->preview = preview;
    impl_->previewDirty = true;
    impl_->redraw = true;
}

void RenderMirror::clearPreview() { setPreview({}); }

DrawList RenderMirror::buildDrawList(const MirrorViewOptions& options) {
    Impl& m = *impl_;
    m.redraw = false;
    if (m.entitiesDirty) m.rebuildEntityBuffer();
    DrawList list;
    list.mode = options.mode;
    list.view = options.view;
    list.grid = options.grid;
    list.edgeOverlay = options.edgeOverlay;
    list.edgeColour = options.edgeColour;
    list.occludedAlpha = m.highlight.occludedAlpha;
    m.resolvePreview();
    const auto previewIndex = [&](NodeId id, DVec3 origin) -> std::uint32_t {
        const auto found = m.effectivePreview.find(id);
        if (found == m.effectivePreview.end()) return 0;
        list.previews.push_back({found->second, origin});
        return static_cast<std::uint32_t>(list.previews.size());
    };
    for (const auto& [id, solid] : m.solids) {
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
            if (const auto material = m.materials.find(batch.material); material != m.materials.end()) {
                draw.texture = material->second.slot;
            }
            switch (options.mode) {
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
                return m.selectedSides.contains(face.side);
            });
            if (faceMode) {
                // Split existing ranges so a face tint does not bleed into its material siblings.
                for (const FaceRange& face : batch.faces) {
                    MeshDraw part = draw;
                    part.firstIndex = face.firstIndex;
                    part.indexCount = face.indexCount;
                    part.firstEdgeIndex = face.firstEdgeIndex;
                    part.edgeIndexCount = face.edgeIndexCount;
                    if (m.selectedSides.contains(face.side)) {
                        part.highlight = m.highlight.selectTint;
                        part.outlineOnlyWire = true;
                        MeshDraw outline = part;
                        outline.colour = m.highlight.outlineColour;
                        outline.highlight = Vec4(0.0f);
                        list.outlines.push_back(outline);
                    }
                    list.meshes.push_back(part);
                }
            } else {
                list.meshes.push_back(draw);
            }
            if (selected) {
                draw.colour = m.highlight.outlineColour;
                draw.highlight = Vec4(0.0f);
                list.outlines.push_back(draw);
            }
        }
    }
    if (options.showEntities && m.entityBuffer && m.entityCount > 0) {
        std::uint32_t instance = 0;
        for (const auto& [id, entity] : m.entities) {
            (void)entity;
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
    return list;
}

const MirrorStats& RenderMirror::stats() const { return impl_->stats; }
void RenderMirror::resetStats() { impl_->stats = {}; }
std::size_t RenderMirror::solidCount() const { return impl_->solids.size(); }
std::size_t RenderMirror::meshedSolidCount() const {
    return static_cast<std::size_t>(std::ranges::count_if(impl_->solids, [](const auto& s) { return s.second.meshed; }));
}
std::size_t RenderMirror::entityBoxCount() const { return impl_->entities.size(); }
std::size_t RenderMirror::batchCount() const {
    std::size_t count = 0;
    for (const auto& [id, solid] : impl_->solids) count += solid.batches.size();
    return count;
}

} // namespace apex::render
