#pragma once

// Render mirror (plan 0.1, 0.1.9 / D4, D6, D12). Keeps a GPU copy of the document's solids and
// point entities, updated from ChangeSets.
//
// Threading: every call is on the document's writer thread (the "main" thread). Dirty solids are
// re-meshed on the JobPool from a private copy of the solid's immutable block (never handles or
// document pointers); completions are applied on the main thread by pump(), which also drains
// the pool (so MaterialCache completions are delivered too). Only solids named by a ChangeSet
// (or whose material just finished loading) are re-meshed.
//
// Granularity: one vertex/index/edge buffer triple per (solid, material) batch, one draw each.
// Retired buffers are destroyed after Context::waitIdle(), so in-flight frames never see them.
//
// Lifetime: the Context, TextureHeap, JobPool, MaterialCache and EntityClassTable must outlive
// the mirror. Destroying the mirror cancels its jobs and releases its texture slots.

#include "assets/fgd.hpp"
#include "assets/material_cache.hpp"
#include "core/job_pool.hpp"
#include "document/document.hpp"
#include "render/renderer.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace apex::render {

struct MirrorStats {
    std::size_t jobsSubmitted = 0;
    std::size_t solidsRemeshed = 0;   // Completed meshes applied (successful or not).
    std::size_t meshFailures = 0;     // Solids that do not form a valid convex polyhedron.
    std::size_t solidsFreed = 0;      // Removed solids whose buffers were released.
    std::size_t texturesUploaded = 0;
};

// Owned IDs; independent of editor selection. Tint alpha controls the colour blend.
struct Highlight {
    std::vector<document::NodeId> selected;
    std::optional<document::NodeId> hover;
    std::vector<document::SideId> selectedSides;
    Vec4 selectTint{1.0f, 0.55f, 0.1f, 0.45f};
    Vec4 hoverTint{0.2f, 0.85f, 1.0f, 0.35f};
    Vec4 outlineColour{1.0f, 0.65f, 0.15f, 1.0f};
    float occludedAlpha = 0.25f;
    bool operator==(const Highlight&) const = default;
};

struct MirrorViewOptions {
    DisplayMode mode = DisplayMode::Textured;
    FrameView view;
    GridSettings grid;
    bool edgeOverlay = false;
    Vec4 edgeColour{0.0f, 0.0f, 0.0f, 1.0f};
    Vec4 wireColour{0.85f, 0.9f, 1.0f, 1.0f}; // Wireframe mode line colour.
    bool showEntities = true;
};

class RenderMirror {
  public:
    RenderMirror(Context& context, TextureHeap& heap, JobPool& jobs, assets::MaterialCache& materials,
                 const assets::EntityClassTable* entityClasses = nullptr);
    ~RenderMirror();
    RenderMirror(const RenderMirror&) = delete;
    RenderMirror& operator=(const RenderMirror&) = delete;

    // Treats every node of the document as created (initial load).
    void rebuild(const document::Document& document);
    // Applies a committed ChangeSet; call after the commit/undo/redo that produced it.
    void apply(const document::Document& document, const document::ChangeSet& changes);

    // Delivers completed jobs and finished material loads, uploads results, and releases retired
    // buffers. Returns true when something visible changed.
    bool pump();
    // Pumps until no mesh job or material load is pending or the timeout passes. Returns true
    // when idle. (Tests and headless tools; an interactive app calls pump() per tick.)
    bool settle(std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // Visible state changed since the last buildDrawList(), or work is still pending
    // (meshing or texture stream-in).
    bool needsRedraw() const;
    // Changes only draw state; never schedules meshing or uploads geometry.
    void setHighlight(const Highlight& highlight);
    bool pending() const;

    // The returned list borrows the mirror's buffers: draw it before the next pump()/apply().
    DrawList buildDrawList(const MirrorViewOptions& options);

    const MirrorStats& stats() const;
    void resetStats();
    std::size_t solidCount() const;     // Tracked solids (including hidden or failed).
    std::size_t meshedSolidCount() const;
    std::size_t entityBoxCount() const;
    std::size_t batchCount() const;     // GPU batches (draws per frame at most).

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apex::render
