# Architecture

This page covers the layer rules, the core model, and the threading rules. Every claim here is
backed by a header or a check script. Where a detail changes often, the page names the file that
owns it instead of repeating it.

## Layers

Code under `src/` is split into layers. A layer may include its own headers and the headers of the
layers it depends on, directly or through another layer. Nothing else.

The direct dependencies, from `DEPS` in [`tools/check_layers.py`](../../tools/check_layers.py),
which mirrors the CMake link graph:

| Layer | Directory | Depends on | Responsibility |
|---|---|---|---|
| `core` | [`src/core/`](../../src/core/) | nothing | Maths, KV1 and KV3 parsing, JSON, paths, logging, job pool, handles, results. |
| `geometry` | [`src/geometry/`](../../src/geometry/) | `core` | Convex hulls, primitives, solid geometry, face UVs, BVH. No document knowledge. |
| `document` | [`src/document/`](../../src/document/) | `geometry`, `core` | The VMF model, transactions, instances, connection codec. |
| `editor` | [`src/editor/`](../../src/editor/) | `document`, `geometry`, `core` | Tools, selection, gizmos, snapping, clipboard, groups, visgroups, autosave. |
| `assets` | [`src/assets/`](../../src/assets/) | `core` | Virtual filesystem, game profiles, VMT, VTF, FGD, indexes, caches, model baking. |
| `render` | [`src/render/`](../../src/render/) | `document`, `assets` | Vulkan context, renderer, swapchain, texture heap, thumbnails. |
| `viewport` | [`src/viewport/`](../../src/viewport/) | `render`, `document` | Cameras, display modes, split layouts, workspaces, picking maths. |
| `ui` | [`src/ui/`](../../src/ui/) | `viewport`, `editor` | Qt widgets, panels, dialogs, the GPU host that puts Vulkan into a Qt widget. |
| `app` | [`src/app/`](../../src/app/) | `ui` | `main()` only. |

Two rules are enforced, and CI runs the check:

1. **Include direction.** A layer may include only its own dependency closure. Violations are
   reported as `layer 'x' must not include 'y'`.
2. **Qt stays up top.** Qt headers (`<Q...>`) are allowed only in `ui/` and `app/`. The layers
   below the UI are plain C++. `core`, `geometry`, `document` and `editor` build and test without a
   GPU or a window. The render tests need a Vulkan device, and lavapipe is the required one.

`tools/check_layers.py` also rejects any use of `processModelData`, which is forbidden because Apex
owns model baking itself. CI runs the check. [../development.md](../development.md) asks you to run
it by hand before each commit.

Note that `editor` sits between `document` and `ui`, which the root README's layer order leaves
out.

## The core model

These are the types a newcomer needs. Each entry gives the header to read first.

**The document** ([`document/document.hpp`](../../src/document/document.hpp)). The map as a tree of
plain nodes. Nodes have document-local IDs that stay stable across undo. A handle to a deleted node
never comes back, even after undo, so stale references fail safely. The codec keeps unknown keys,
duplicate keys, byte spans and the original spelling of every value. That is why saving an
unmodified file gives back the same bytes.

**The VMF layer** ([`document/vmf.hpp`](../../src/document/vmf.hpp)). A typed view over KV1 nodes.
A setter dirties only the value it changes, and the writer copies everything else verbatim. This is
how Apex keeps round-trips with Strata Hammer lossless.

**Transactions.** Every change to the document goes through a transaction. The transaction is what
undo and the history panel record, and what the viewport, outliner and inspector use to find out
what to refresh. The rule from the root README is: "renderer, picking and outliner update from its
change set". Do not mutate the document from outside a transaction.

**Solids and geometry** ([`geometry/solid_geometry.hpp`](../../src/geometry/solid_geometry.hpp)).
A solid is a set of planes and sides. Geometry is rebuilt from the planes, and `point_data` is
reconciled against them. The geometry layer does not know about entities or the editor.

**The editor session** ([`editor/editor_session.hpp`](../../src/editor/editor_session.hpp)). Owns the
document together with the session state that is not saved: the selection
([`editor/selection.hpp`](../../src/editor/selection.hpp)), the active tool, the pick index, and
autosave. Selection is session state, so selecting something never writes to the document.

**Tools** ([`editor/tool.hpp`](../../src/editor/tool.hpp)). A tool receives pointer and key events in
logical pixels, together with a ray, a projection and a work plane. It does not receive "2D" or "3D"
concepts, and it cannot see a widget. Every task must be doable in a 3D viewport, so tools are
written against the ray. The concrete tools are the `*_tool.cpp` files in `src/editor/`, for example
`select_tool.cpp` and `clip_tool.cpp`.

**Viewports and layouts** ([`viewport/viewport.hpp`](../../src/viewport/viewport.hpp)). A viewport is
a camera, a display mode and a set of overlays. A "2D" view is an orthographic, axis-locked
wireframe viewport, so there is one viewport type. A layout is a split tree of areas
([`viewport/layout.hpp`](../../src/viewport/layout.hpp)). A workspace is a named layout that the
user switches between, as in Blender ([`viewport/workspace.hpp`](../../src/viewport/workspace.hpp)).

**The renderer** ([`render/renderer.hpp`](../../src/render/renderer.hpp)). One `Context` per process
holds the Vulkan instance, device, graphics queue and allocator. Every other GPU object borrows the
context and must be destroyed before it. The renderer uses dynamic rendering, one shared pipeline
layout, a bindless texture heap, and reversed-Z depth. Positions are camera-relative: each draw
carries a double-precision world origin that is rebased against the camera, so large maps do not
jitter.

**Assets** ([`assets/vfs.hpp`](../../src/assets/vfs.hpp),
[`assets/game_profile.hpp`](../../src/assets/game_profile.hpp)). The virtual filesystem stacks
mounts; the earlier mount wins. It finds Steam installs and reads VPK archives. A game profile
describes one game: its `gameinfo`, its mounts and its addons. The SourcePP libraries (`vpkpp`,
`steampp`, `vtfpp`, `toolpp`, `mdlpp`) are used only inside this layer. Their types never cross
the `Vfs` boundary.

**Indexes and caches.** Material, model, sound, particle and general asset indexes are built by
background jobs. Their results come back to the owner thread. The index classes are the
`*_index.hpp` files in `src/assets/`. The caches on disk are `material_cache`, `model_cache` and
`thumbnail_cache`.

**Model baking.** `assets/model_bake.cpp` and `model_pose.cpp` turn MDL data into what the renderer
draws. Apex owns this step on purpose, and the upstream convenience bake is forbidden by the layer
check.

**The UI** ([`ui/main_window.hpp`](../../src/ui/main_window.hpp)). `MainWindow` hosts the panels
(outliner, inspector, history, log, visgroups, asset browsers, and so on) and the viewport areas.
`gpu_host` is the bridge that gets Vulkan rendering into a Qt window. Read a panel's header for
what it owns before you change it.

## Threading

- **The main thread** owns the Vulkan device, the `Vfs`, the document and the UI. The renderer
  design (plan 0.1, D12) is one `VkDevice` and one graphics queue, used from the main thread only.
- **Background work** runs on `core/job_pool.hpp`. Workers parse owned snapshots, never live
  document or filesystem state.
- **Index builders** read bytes on the owner thread, hand parsing to workers, and apply results
  when the owner drains completions. The `asset_index.hpp` header states the exact rule.

## One interaction, traced

This is a sketch of the path a mouse action takes. Check the named files before relying on any
detail.

1. A Qt mouse event reaches a viewport widget in [`ui/viewport_pane.cpp`](../../src/ui/viewport_pane.cpp)
   or [`ui/viewport_area.cpp`](../../src/ui/viewport_area.cpp). It is converted to logical pixels, with
   the source pane recorded, as `tool.hpp` requires.
2. The viewport turns the pixel into a ray with the picking maths in
   [`viewport/pick_math.hpp`](../../src/viewport/pick_math.hpp). The UI passes that ray to the
   editor as a callback, because `editor` may not include `viewport`.
3. The active tool (for example `select_tool.cpp`) decides what the click means. A drag may start a
   modal transform ([`editor/modal_transform.hpp`](../../src/editor/modal_transform.hpp)) or a gizmo
   ([`editor/gizmo.hpp`](../../src/editor/gizmo.hpp)). Snapping comes from
   [`editor/snapper.hpp`](../../src/editor/snapper.hpp). Previews are built in
   [`editor/preview.hpp`](../../src/editor/preview.hpp).
4. On commit, the tool opens a transaction. The document changes, and geometry is rebuilt from the
   changed solids.
5. The change set reaches the renderer, which re-uploads the affected meshes, and the outliner,
   inspector and history panels, which refresh. Undo and redo work from the same transaction
   history.

## Where the design rules live

The rules behind this structure are in the root [README](../../README.md#design-rules) and in
[`docs/plans/0.1.md`](../plans/0.1.md), section 2, which fixes the seam between layers. Read that
plan section before you move code between layers.
