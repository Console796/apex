# Apex roadmap

**Status: reviewed by the project owner, 2026-10-20.** Long-range plan for 0.1 → 0.10. The milestone order was confirmed in review (lighting stays at 0.6/0.7). The spec is
[research.md](research.md); where they disagree, the spec wins until amended. Each milestone gets
its own plan in `docs/plans/` before work on it starts. The first is [plans/0.1.md](plans/0.1.md).

## Versioning

`MAJOR.MILESTONE.SUB.BUILD`, starting at **0.0.0.0**.

| Field | Meaning |
|---|---|
| MAJOR | `0` until the 1.0 release criteria (end of 0.10) are met. |
| MILESTONE | Feature milestone, 1–10 in this plan. |
| SUB | Sub-milestone within it. `SUB = 0` means "milestone planned, nothing landed yet". Each sub-milestone is a reviewable, mergeable unit with its own exit criteria. |
| BUILD | Monotonic run counter of the local CI runner `tools/ci.py` (0.1 plan, D1/D22). Ad-hoc builds report `0`. |

The version names the **last completed sub-milestone**. When 0.1.10 lands, 0.1 is done, and the
next landed work is 0.2.1. Git tags (`v0.1.3`) mark completed sub-milestones and carry no build
number.

**Status:** 0.1 is complete and 0.2.4 has landed (`version.txt` reads 0.2.4). The next landed
work is 0.2.5 (see `plans/0.2.md`).

## Principles that hold across every milestone

1. **Every task is doable in a 3D viewport.** Users can open any number of viewports and switch
   each one's projection (perspective/orthographic) and display mode (wireframe, flat, textured,
   lit). A "2D view" is a preset of the same viewport type: an axis-locked orthographic camera with
   wireframe display. Tools never branch on "2D vs 3D". They see a ray, a projection kind and a
   work plane.
2. **VMF interop with Strata Hammer is a hard requirement.** Files go both ways without loss.
3. **A pure core.** Document, geometry and asset code build and test with no GPU and no UI.
4. **Everything is a transaction.** No tool mutates the document outside one, and every consumer
   (renderer, picking, outliner, lighting) updates incrementally from the change set it emits.
5. **Game specifics live in profiles.** Strata/P2CE is the only profile through 0.10, but its
   quirks (`point_data`, KV escapes, BSP v25, `light_rt`, mounts) are isolated so other branches
   can be added later.
6. **Relicensable.** MIT now, with **GPLv3-or-later** as the only planned relicensing target: no
   dependency is GPL or GPLv3-incompatible. GPLv2 is deliberately not kept open (0.1 plan, D4).

## Milestones

Each milestone lists its goal, scope, and the exit criteria that close it. Its plan breaks it into
sub-milestones.

### 0.1: Foundations: open, view, save
Build system, CI (Linux + Windows), pure core (math, handles, KV1), VMF document model with
transactions and undo, brush kernel (construction and validation), game profile + virtual
filesystem (gameinfo, mounts, VPK), VMT/VTF/FGD loading, Vulkan renderer foundation, multiple
user-configurable viewports, and the main window shell.
**Exit:** every VMF in P2CE's `sdk_content` opens; brushes render textured in any number of
viewports, each switchable between perspective/ortho and wireframe/flat/textured; saving an
unmodified file is byte-identical; `tools/ci.py` is green on the Linux presets and in the Windows
VM, with validation-layer-clean offscreen render tests and the full P2CE corpus.

### 0.2: Selection & manipulation
Picking (BVH over faces, entities and models), object/face/solid selection modes, marquee/frustum
select, gizmos (translate/rotate/scale, local/global, pivot) with snapping, surface-anchored grid
and work planes, undo history panel, clipboard/duplicate/paste-special, groups, visgroups,
hide/isolate, outliner (editable), FGD-driven entity property inspector, point-entity placement on
surfaces, autosave + crash recovery.
Scope adjustments after 0.1 (plan 0.1.10): the viewport split tree, per-viewport header menus,
`ViewContext`/`WorkPlane` types, the log console, status bar, theme tokens and a **read-only**
outliner are already in 0.1. 0.2 therefore starts from them: it wires tools to `ViewContext`,
makes the outliner editable, and builds the inspector. Face selection uses `SideId` (plan 0.1
D21), never a face index. Nothing else from 0.2 slipped into 0.1.
**Exit:** an existing map can be rearranged and its entity keyvalues edited entirely in 3D, and
the result opens in Strata Hammer unchanged except for the edits.

### 0.3: Brush authoring in 3D
Block tool (draw on a surface or work plane, then extrude), primitives (wedge, cylinder, arch,
spike, sphere), clip tool (3 points placed on surfaces), vertex/edge/face tool with
convex-hull rebuild and invalid-state feedback, carve/hollow/merge, material browser,
texture application and face editing (shift/scale/rotate, justify/fit, align to world/face,
texture lock under transforms), measure tool.
**Exit:** a complete test chamber can be built from nothing without an orthographic view.

### 0.4: Entities & assets
MDL rendering (v44–49: skins, bodygroups, non-uniform scale), FGD helpers (iconsprite, studio,
sphere, line, frustum, cylinder, worldtext, catapult…), I/O connection editor with validation
and autocompletion, `func_instance` rendering, in-context instance preview and collapse, model,
sound and particle browsers, prefabs, entity report, find & replace.
**Exit:** a chamber can be fully dressed with props and wired logic.

### 0.5: Compile & play
Compile profiles (including import of Hammer `.wc` via toolpp), vbsp/vvis/vrad/postcompiler as
cancellable background jobs using the **native Linux tools** on Linux, log parsing with
error → object mapping, `.lin` leak and `.prt` portal overlays, cordons, map validation
("check for problems"), launch the game with `-map`.
**Exit:** the edit → compile → play loop works end-to-end on Linux and Windows.

### 0.6: Lighting I: realtime preview
A light model shared with the baker: `light`, `light_spot`, `light_environment`, and `light_rt`
with direct/indirect/specular modes, using Source falloff formulas exactly. Clustered forward+
with a shadow atlas, sun and sky ambient, HDR and tone mapping matched to the game, fog,
2D and 3D skybox preview, better LightmappedGeneric/PBR/WorldVertexTransition fidelity, cubemap
preview.
**Exit:** direct lighting matches in-game captures within an agreed per-pixel tolerance on
reference scenes, and `light_rt` dynamic lights match the game.

### 0.7: Lighting II: baked preview
Luxel charting matching VBSP (`lightmapscale`, face splits, BSP v25 limits), a progressive GPU
baker (Vulkan ray query) with an Embree CPU fallback, bounce using VTF reflectivity, bumped
lightmaps, sky, OIDN denoise, incremental invalidation with camera-priority scheduling, a
background VRAD round-trip mapped back through `hammerfaceid`, an A/B compare view, and a luxel
density view.
**Exit:** preview vs VRAD error is under a threshold on the reference maps, and the preview
re-converges interactively after moving a light.

### 0.8: Advanced geometry & surfaces
Displacements (create, sculpt, paint alpha/multiblend, sew, subdivide), overlays
(`info_overlay`, face sets), hotspot texturing (VTF hotspot resource + rect maps), decals,
rope and cable preview, detail/structural tooling.
**Exit:** organic and outdoor geometry workflows reach parity with Hammer++.

### 0.9: Extensibility
Plugin API (stable C ABI with a header-only C++ wrapper), a scripting host (language decided in
the 0.9 plan), custom tools/panels/importers, keymap editor, theme editor, addon manager,
scriptable primitives.
**Exit:** a third-party tool can be built, loaded and shipped without rebuilding Apex.

### 0.10: Production hardening → 1.0 RC
Performance budgets on the largest available maps, an interop test suite against Strata Hammer,
user manual, installers (AppImage/Flatpak, Windows installer), localization scaffolding, an
accessibility pass, and a bug bash.
**Exit:** the 1.0 release criteria, which are written at the start of 0.10.

## Ordering rationale

Lighting, the flagship feature, arrives at 0.6/0.7 because each tier needs what comes before it:
- tier 1 needs placeable, editable light entities (0.2/0.4) and the material/shader fidelity work;
- tier 3 needs the compile pipeline (0.5);
- tier 2 needs both, so it can be measured against tier 3.

Building lighting earlier would mean building it against stand-ins and rebuilding it later.
0.1 still has to get the seams right for it: the renderer's bindless material and texture layout,
the requirement for ray-query-capable devices, and the change sets that drive incremental
invalidation.
