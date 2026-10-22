# Apex: research notes for a Strata Source level editor

Target: Portal 2: Community Edition (Strata Source). Secondary: other Strata games (Portal: Revolution, Momentum Mod), then older Source branches through a "game profile" layer.

Requirements: clean modern UI, editing done entirely in 3D viewports, lightmap preview before compiling (realtime or progressive, like Hammer++), and a robust, modular design.

---

## 1. What Strata Source changes for an editor

From the Strata wiki source (github.com/StrataSource/Wiki) and the VDC Strata page:

| Area | Strata specifics | What it means for the editor |
|---|---|---|
| Platform | 64-bit only, Windows + Linux, macOS dropped. Native DX11; on Linux it runs on Vulkan through DXVK. **The compile tools (vbsp/vvis/vrad/studiomdl) ship native Linux builds** in `bin/linux64/`. | A native cross-platform editor can drive the real compilers on both OSes without Wine. No need to support macOS/Metal. |
| BSP | **v25.** `MAX_COORD_INTEGER = 65536`, so the world spans ±65536 on every axis (131072 total). Faces, nodes, leafs, overlays (v3 is variable-size), worldlights (v2, with `lightmode`) and static props (v13, non-uniform scale vector) are all re-laid out. Lump `dfaceid_t { hammerfaceid }` is still present. | Do brush math in **double precision** (float precision at 65k is about 0.004 units). `hammerfaceid` maps compiled faces back to VMF side IDs, which the lighting round-trip relies on (§4). |
| VMF | Plane triples as before, plus an optional Strata `point_data { numpts; point "i x y z" ... }` block on each `side` holding exact vertices (stops off-grid brush drift). Viewport positions and `nInstanceVisibility` are saved in `viewsettings`. KV strings support `\n \t \" \\` escapes. A note is written at the top of the file when Strata extensions are used. | Read and write `point_data`, and treat it as the source of truth when it's present. Round-trips must be lossless and keep unknown keys, because users will switch between your editor and Strata Hammer. **Corpus facts (512 P2CE `sdk_content` VMFs, checked in 0.1):** side IDs live in their own ID space and collide with solid/entity IDs in 180 files, so the two need separate allocators; an object in several visgroups repeats `editor { visgroupid }` (49 blocks in 6 files), so keyvalue stores must be ordered multimaps; line endings are mixed (510 LF, 2 CRLF) and are preserved per file; a plural `cordons` block is used (not `cordon`); and `"editorextension" "strata"` in `versioninfo` marks Strata-saved files, which Apex keeps and never replaces (Strata Hammer rewrites any other value, and drops unknown `versioninfo` keys and top-level blocks). Untouched content is written back byte-for-byte (plan 0.1 D7, D18, D19). Further corpus and probe facts from 0.2: groups are `group` blocks and membership is per-object `editor { groupid }`; hidden objects are wrapped in `hidden { }` blocks (Hammer reorders them on save, an accepted cost); exponent-notation numbers occur in plane points and are rewritten in fixed notation only for edited values; the Strata Hammer probe of hide, visgroup visibility and the clipboard is recorded in `plans/probe-results` and plan 0.2 D10, D11, D12 and D18. |
| Lighting | Clustered forward+ dynamic lighting (`light_rt`) uses shadow-map atlases. Each light has separate **Direct / Indirect / Specular** modes (None / Static / Dynamic). Worldlight lump modes are `static`, `specular`, `bounce`, `dynamic`. Light cookies, volumetrics, parallax-corrected cubemaps. VRAD adds portal-traversal lighting, baked AO (or runtime XeGTAO), `_removeaftercompile`, texture shadows, `-StaticPropLighting`, light culling. | The preview has to show **two** lighting systems: baked lightmaps (VRAD) and clustered dynamic lights. A realtime shadow-mapped renderer covers the dynamic half for free (§4). |
| Materials | VMT (KV1). LightmappedGeneric, VertexLitGeneric, WorldVertexTransition, plus Strata **PBR** (MRAO, parallax occlusion). `%ToolViewportTexture` swaps in a different material in the editor viewport. | You need a VMT→editor-shader translation layer covering at least these shaders. |
| Textures | **VTF 7.6** adds BC7, BC6H, R8, and per-mip/frame/face CPU compression (Deflate or Zstd, `AXC` resource v1/v2). Hotspot rects can be embedded in the VTF (resource tag `+\0\0`) or come from a text format / Hammer++ rect maps. | Use vtfpp (it supports 7.6). Hotspot texturing is expected now, since Strata Hammer and Hammer++ both have it. |
| Filesystem | `gameinfo.txt` SearchPaths **plus** a Strata `mount` block (mounts other Steam games' content, e.g. Portal 2) and user `cfg/mounts.kv`; `custom/*` globbing; `-mountmod`. | You have to implement the mount system to resolve P2CE assets. SourcePP's `fspp` is marked incomplete. |
| Entities | FGDs live in StrataSource/FGD, a fork of TeamSpen's HammerAddons unified FGD database. Strata Hammer also loads AngelScript "scriptable primitives" from `hammer/scripts/*.as`. | `toolpp` parses Strata-flavoured FGD. Scripted-brush compatibility is a possible later feature. |
| Models | studiomdl accepts SMD/DMX/FBX/OBJ. Strata loads Source 2013 models, and P2/CS:GO MDLs natively. | mdlpp reads MDL v44–49 but is read-only and incomplete. **Check it against real P2CE content early.** |

### Strata Hammer (what you're competing with)
It's a Qt 6 fork of Hammer. It has: Qt themes (`-theme`), Blender-style middle-mouse camera, hotspotting, AngelScript scriptable brushes, an addon manager, sound/scene/particle/soundscape browsers, catapult visualizer, saved brush vertices, and a lighting preview. Their tracker has open bugs against that preview (StrataSource/Engine #922, #95: ghosting, too dark compared with in-game, viewport layering). The editor is still built around 2D views, and its source is closed.

### Hammer++ feature bar (ficool2)
- Lighting preview with configurable quality/speed and live updates (in practice: realtime direct light, and only brushes cast shadows).
- Realtime vertex tool that highlights invalid faces; precise vertex storage; 3-point clipping with 3D handles; merge-to-convex; polygon tool.
- Global/local gizmo with pivot; drop objects to floor or surface aligned to its normal; sprinkle tool; physics simulation tool.
- Create instance from selection; instance preview inside the parent map.
- 3D skybox, fog, rope, particle, and material-proxy preview; luxel-density tint.
- Model/material hot reload; microgrid down to 0.125; multiple cordons; keybind editor; crash recovery.

---

## 2. Prior art and reusable pieces

**Editors (none is a production-grade open-source Source 1 editor):**
- **TrenchBroom** (C++/Qt, GPL-3.0, very active). This is the best design reference: it started as a 3D-only editor, and its vertex tool, surface-anchored grid and brush-validity rules are what to study. Because it's GPL, read its approach but don't copy code unless your project is GPL too.
- **rathammer** (Zig, VMF editor, active in 2026, now on sourcehut). Worth a look as the closest contemporary attempt.
- Lambda (Go, stalled, incomplete), Sledge (C#, dead, GoldSrc), Chisel (Godot 3 stub), MRVN-Radiant (NetRadiant fork for Titanfall/Apex).

**Libraries:**
- **SourcePP** (MIT, C++20, active): `vpkpp` (VPK v1/v2/v54 read/write), `vtfpp` (VTF 7.0–7.6 incl. Strata), `bsppp` (BSP v17–27 incl. Strata, read/write), `toolpp` (FGD incl. Strata/Hammer++/J.A.C.K. extensions, and `.wc` compile configs), `kvpp` (KV1 text/binary), `mdlpp` (read-only, incomplete), `steampp` (find Steam libraries and games). C/C# bindings exist for vpkpp/vtfpp/steampp; Python for most.
- **srctools** (Python, TeamSpen): the most complete VMF model available. It already handles Strata `point_data`, viewports and instance visibility, plus FGD, VMT, instance collapsing, and the HammerAddons postcompiler. Good **test oracle**, and you can use the postcompiler as-is in your compile pipeline.
- **BSPSource** (Java): BSP→VMF decompiler, useful for building a test corpus.
- Test content: StrataSource/P2CE-Instances, StrataSource/sample-content, p2ce-mod-template.

---

## 3. What building it involves (subsystems)

1. **Document model + persistence.** Lossless VMF read/write (keep unknown blocks and keys), ID allocation, groups, visgroups, cordons, instances (`func_instance`, parameter fixup), `point_data`. Store everything in arenas with stable IDs, and represent every mutation as a command that produces a reversible diff. Undo/redo, autosave and crash recovery fall out of that.
2. **Geometry kernel (double precision).** Convex brushes as plane sets → windings → polygons; validity checks; vertex/edge/face editing that keeps brushes convex (TrenchBroom's split/merge rules); clip, carve, hollow, merge-to-hull; Valve 220 texture axes (`uaxis`/`vaxis`, scale, shift, rotation), justify/fit/align, texture lock under transforms; hotspot fitting. **Displacements** are their own subsystem: power 2–4, sewing, normals/offsets, alpha, multiblend, sculpt brushes. **Overlays**: basis, UV points, face lists.
3. **3D-only interaction.** This is the hardest part to get right. You need:
   - BVH picking over faces, entities and model triangles.
   - A **grid that is a world-space plane anchored to the hovered surface or the current work plane**.
   - Draw-on-surface-then-extrude brush creation.
   - Gizmos with snapping.
   - Clip-plane placement by clicking points on surfaces.
   - Frustum/marquee selection, isolate/hide, measurement.
   - Fly, orbit and Blender-style camera modes.
   - Axis-locked **orthographic cameras inside the same 3D viewport** for precision work. These are still 3D views, not Hammer's 2D views.
4. **Asset system.** gameinfo + Strata mounts + VPK + loose files, async loading with caches, file watching for hot reload, Steam library discovery.
5. **Rendering.** VMT→shader translation (generic, blend, PBR approximation, alpha-test, translucency, tool textures); MDL rendering (skins, bodygroups, non-uniform scale); FGD helpers (iconsprite, studio, sphere, line, frustum, cylinder, worldtext, swept hulls…); selection outlines; GPU-driven culling and instancing; background-thread mesh rebuilds so large maps stay responsive.
6. **Entities.** FGD → generated property inspector (choices, flags, color, studio/material/sound pickers, target autocompletion), I/O connection editor with validation, instance I/O.
7. **Compile pipeline.** Drive vbsp/vvis/vrad (+ postcompiler) as cancellable background jobs; parse progress; overlay `.lin` leak files and `.prt` portals; map errors ("brush id N") back to selectable objects; launch the game with `-map`.
8. **Lighting preview** (§4).
9. **Extensibility.** Plugin API for tools, panels, importers and entity helpers, plus a scripting layer (see §6).

---

## 4. Lighting preview design

Build it in three tiers that share one light model, so the viewport can switch between them and they all agree.

**Tier 1: realtime direct (Hammer++-level).** Clustered forward+ with shadow maps. Reproduce Source's light formulas exactly:
- constant/linear/quadratic attenuation;
- `_fifty_percent_distance` / `_zero_percent_distance`, `_hardfalloff`;
- spot inner/outer cone and exponent;
- `light_environment` sun with `SunSpreadAngle` and ambient;
- HDR brightness keys.

This tier also *is* the accurate preview for Strata `light_rt` dynamic/specular modes, since the game renders those the same way.

**Tier 2: progressive lightmap bake on the editor's own geometry.**
- **Chart luxels the way VBSP does**: lightmap axes come from each face's texture axes scaled by `lightmapscale`, and the BSP v25 limits apply (128 luxels for brushes, 509 for displacements).
- Path-trace direct + indirect per luxel, accumulating progressively, then denoise.
- Bake **bumped lightmaps** (flat + Source's 3-vector bump basis) so normal maps look right.
- Use HDR storage.
- For bounce albedo, use the **VTF reflectivity vector** (it's in the VTF header, and VRAD uses it).
- Sky: sample the `light_environment` ambient over the hemisphere, matching VRAD's sky rays.
- Invalidate incrementally: moving a light dirties only luxels inside its range; editing a brush re-charts only the faces it touches. Bake luxels visible to the camera first.
- Backends: Vulkan `VK_KHR_ray_query` compute (NVIDIA RTX, AMD RDNA2+, Intel Arc; RADV/ANV support it), with **Embree 4** (CPU, Apache-2.0) as the fallback. Denoise with **OIDN 2** (Apache-2.0, CPU + GPU). Reference: *glim* (open-source Vulkan ray-query lightmapper with OIDN).

**Tier 3: ground truth by round-trip through the real VRAD.**
- In the background: export the VMF → `vbsp` → (`vvis -fast` or skip) → `vrad` with chosen flags.
- Read the lighting lump with `bsppp`, and use **`hammerfaceid`** to attach compiled lightmaps back onto editor faces. VBSP splits faces, so draw the BSP fragments in place of the editor face until it's edited again.
- This gives exact parity (portal traversal, AO, texture shadows) while the user keeps working. It also lets you show tier 2 next to tier 3 to tune the baker against VRAD.

Parity risks: VRAD's radiosity (patch chop, supersampling, smoothing groups), bump-basis conventions, tone mapping between the editor and the game, and Strata-specific additions whose formulas aren't public (portal traversal, AO). Tier 3 covers all of them.

---

## 5. Tech-stack options

### Option A: C++20/23 + Qt 6 + Vulkan  ← **recommended**
- **UI**: Qt 6 Widgets with a custom style plus **Qt Advanced Docking System** (LGPL-2.1) for docking. Use QML/QQuickWidget only for panels that benefit from animation. *(Decided in 0.1, D16: Widgets with a design-token style and Qt-ADS for the side panels, no QML. Viewports live in a central split-tree area, not in dock widgets, because native Vulkan windows inside docks fail on Wayland; D15.)* *(Correction: KDDockWidgets is GPL-2.0/GPL-3.0 or commercial only, so it can't be used in an MIT project.)*
- **GPU**: Vulkan 1.3 (dynamic rendering, VMA, `ray_query`) behind a thin internal RHI. *(Decided in 0.1, D13: raw Vulkan 1.3 with a thin in-house wrapper, not NVRHI and not QRhi. NVRHI (MIT) and Diligent Engine (Apache-2.0) remain the fallback if a D3D12 path ever becomes necessary.)*
- **Libraries**: SourcePP directly, Embree 4, OIDN 2, a job system (Taskflow or enkiTS), CMake + vcpkg or CPM, Catch2/doctest.
- **Pros**: SourcePP is the only maintained library with Strata format coverage, and it's C++20. That reason alone tips the choice. Qt is the most mature desktop toolkit for dense tool UIs (trees, property grids, docking, IME, accessibility); Strata Hammer and TrenchBroom both validate it. Full low-level GPU access for the baker. Same language as the engine ecosystem.
- **Cons**: you own memory safety (lean on ASan/UBSan, RAII and arenas). Build complexity. Qt is LGPL, so link it dynamically or buy a commercial licence.

### Option B: Rust + wgpu + egui (or Slint)
- Viewport on wgpu; baker on wgpu's `EXPERIMENTAL_RAY_QUERY` or a separate `ash` (raw Vulkan) path; egui + egui_dock for the shell.
- **Pros**: memory safety, cargo/crates make modularity easy, fearless multithreading for jobs and mesh building, wgpu is very portable.
- **Cons**: wgpu ray tracing is still marked experimental. egui reads as "tool UI" rather than polished, with weaker text, IME and accessibility. Slint looks nicer but is less proven for docking-heavy apps. You'd rewrite the format code (with srctools and SourcePP as specs) or go through SourcePP's C bindings, which don't cover BSP/FGD. Editor data is graph-shaped, so you'd need arenas or ECS to keep the borrow checker happy.

### Option C: C# (.NET 9/10) + Avalonia 11 + Silk.NET Vulkan
- **Pros**: fastest route to a *good-looking* modern UI (Fluent theme, data binding, generated property grids); easy plugins (AssemblyLoadContext); vpkpp/vtfpp/steampp have C# bindings; fast iteration.
- **Cons**: embedding a Vulkan viewport in Avalonia takes work (NativeControlHost or GPU interop); GC must stay out of the frame loop; Embree/OIDN/BSP/FGD/MDL go through P/Invoke or rewrites; a split managed/native architecture.

### Option D: C++ + Dear ImGui (docking)
- **Pros**: quickest prototype of a 3D-centric editor. Strata itself forks ImGui.
- **Cons**: hard to make look like a "clean modern UI" with large property-heavy panels; weaker text, IME and accessibility. Good for an internal prototype or debug overlays inside Option A; not recommended as the final shell.

### Not recommended
- **Web UI shell** (Tauri/Electron/CEF over a native viewport): compositing DOM with a native GPU viewport and keeping input latency low is a constant fight.
- **Building inside Godot/Unreal**: you get UI and rendering for free, but you fight the engine on double-precision brush CSG, VMF fidelity and VRAD-matching lightmaps.

| | A: C++/Qt/Vulkan | B: Rust/wgpu/egui | C: C#/Avalonia | D: C++/ImGui |
|---|---|---|---|---|
| Strata format coverage on day 1 | ★★★ SourcePP native | ★ rewrite/FFI | ★★ partial bindings | ★★★ |
| Modern-looking UI | ★★ (needs styling work) | ★ / ★★ (Slint) | ★★★ | ★ |
| GPU ray-traced baking | ★★★ | ★★ (experimental) | ★★ (via native) | ★★★ |
| Safety / refactorability | ★ | ★★★ | ★★★ | ★ |
| Iteration speed | ★★ | ★★ | ★★★ | ★★★ |
| Ecosystem precedent | Hammer, Strata Hammer, TrenchBroom | — | Sledge | — |

---

## 6. Modular architecture (any stack)

```
apex/
  core/        math (double), Handle<T>, KV1 codec, Result/Error, logging, JobPool, string/path utils   — pure, headless-testable
  document/    VMF schema, document arenas + generational handles, transactions/undo, ChangeSets, lossless VMF codec — pure
  geometry/    brush kernel (convex polyhedron), CSG ops, texture axes, displacements, overlays, hotspot fit, plane matching — pure
  assets/      Vfs (gameinfo + Strata mounts + VPK, case-insensitive), VMT/VTF/FGD/MDL adapters over SourcePP, game profiles — no Qt, no GPU
  render/      Vulkan device/swapchains, bindless heap, material translation, render mirror, picking BVH, helpers — no Qt
  viewport/    Viewport, Camera, controllers, DisplayMode, ViewContext, WorkPlane, layout model (split tree as data) — no Qt
  editor/      EditorSession: selection, picking index, work planes, gizmos, modal transforms, clipboard, groups, visgroups, hide, inspector model, placement, autosave — no Qt (plan 0.2 D1)
  ui/          Qt shell: main window, Qt-ADS panels, viewport host windows, inspectors, browsers, themes, keymap
  app/         executable: startup, profile selection, wiring
  (later milestones, above document/render as needed)
  lighting/    ILightBaker { realtime | progressive(GPU RT / Embree) | vrad-roundtrip }, shared light model
  compile/     job runner for vbsp/vvis/vrad/postcompiler, log parsing, .lin/.prt
  tools/       one module per tool implementing ITool (input, overlay drawing, emits commands)
  plugins/     public C ABI (or C++ ABI-stable facade) + scripting host
```

Layer stack as built in 0.1 (plan 0.1 §2) and extended in 0.2 (plan 0.2 D1), bottom to top: `core`, `document`/`geometry`, `assets`, `render`, `viewport`, `editor`, `ui`, `app` (`editor` depends on `document`, `geometry` and `core` only, and is Qt-free). Solids are stored as VMF blocks with geometry derived from them (plan 0.2 D4), and selection is keyed by `NodeId`/`SideId`, never by handle (plan 0.2 D3). Dependencies only point downward, and `tools/check_layers.py` enforces it on `#include`s. `document` depends on `geometry` (a solid's value type is a polyhedron), `assets` depends on `core` only, and `render` is Qt-free. Game profiles live in `assets/`.

Principles:
- **Everything flows through commands.** Tools never touch the document directly. The document publishes change events; the render mirror, picking BVH and lighting invalidation all subscribe to those events and update incrementally.
- **A face's identity is its VMF side ID, never its index.** Every face carries a `SideId` (a separate ID space from solids and entities). Selection, overlay `sides` lists, undo references and change-set dirty flags use it. Geometry rebuilds (vertex edit, clip, carve, merge) pair new faces with old ones by plane matching, so matched faces keep their `SideId`, material and UV axes, and unmatched ones get a fresh ID (plan 0.1, D21).
- **Game specifics live in profiles.** BSP v25, `point_data`, `light_rt` modes and the mount system are Strata-profile behaviour, so other branches can be added later without `if (strata)` scattered everywhere.
- **Scripting.** Options: AngelScript (matches Strata Hammer, and could eventually load their `hammer/scripts/*.as` brushes if you mirror the API), Lua (small, fast), or Python (the srctools ecosystem). Embedding Python opens up srctools-based automation, which the community already uses heavily.
- **Testing.** Build a corpus (P2CE-Instances, sample-content, decompiled maps) and gate CI on VMF round-trips byte-identical modulo formatting, with srctools as a second parser to cross-check. Add golden images for the renderer, and a tier-2 vs tier-3 lightmap error metric for the baker.

---

## 7. Suggested milestones

1. **M0, headless core:** KV1/VMF codec, document model, undo, brush kernel; round-trip the corpus.
2. **M1, viewer:** filesystem + mounts, VPK/VTF/VMT, brush and model rendering, camera modes.
3. **M2, editing basics:** picking, selection, gizmos, FGD-driven entity inspector, I/O editor.
4. **M3, 3D brush tools:** surface grid, block draw/extrude, clip, vertex tool, texture application and alignment.
5. **M4, compile pipeline:** vbsp/vvis/vrad jobs, leak/portal overlays, run game.
6. **M5, lighting tier 1:** realtime direct + `light_rt`.
7. **M6, lighting tiers 2 and 3:** progressive baker, VRAD round-trip, comparison view.
8. **M7, advanced geometry:** displacements, overlays, instances (with preview), hotspotting.
9. **M8, extensibility:** plugin API, scripting, theming, keymap editor.

## 8. Open questions to settle early
- **License. Closed (0.1, D4): MIT now, relicensable to GPLv3-or-later.** Only MIT, BSD, zlib, Boost, Apache-2.0, MPL-2.0 and dynamically linked LGPL dependencies are allowed; GPL of any version is not (this rules out KDDockWidgets), and GPLv2 is deliberately not kept open. That stays compatible with SourcePP, Embree and OIDN, and still rules out copying TrenchBroom (GPL). Don't base anything on leaked Valve code. Source SDK 2013 code (including VRAD) is under Valve's SDK licence, so re-derive formulas from public documentation where you can.
- **Does mdlpp handle P2CE's shipped MDLs?** Check on day one; if not, budget a model loader.
- **Is any 2D view needed at all? Closed (0.1, D15).** There is one viewport type; a "2D view" is a preset of it (axis-locked orthographic camera, wireframe, grid). Tools see a `ViewContext` (ray, projection kind, work plane), never a view kind.

## Sources
- Strata wiki source: https://github.com/StrataSource/Wiki (bsp-v25.md, vtf-v76.md, gameinfo.md, vrad.md, vbsp.md, clustered/what_is.md, angelscript/hammer/*, P2CE changelogs)
- Strata Source (VDC): https://developer.valvesoftware.com/wiki/Strata_Source
- Valve Hammer Editor (VDC): https://developer.valvesoftware.com/wiki/Valve_Hammer_Editor
- Hammer++ features: https://ficool2.github.io/HammerPlusPlus-Website/features.html
- SourcePP: https://sourcepp.org/
- srctools: https://github.com/TeamSpen210/srctools
- Strata lighting preview issues: https://github.com/StrataSource/Engine/issues/922, https://github.com/StrataSource/Engine/issues/95
- wgpu ray tracing status: https://wgpu.rs/doc/wgpu/documentation/extensions/ray_tracing/index.html
- glim lightmapper: https://github.com/clienthax/glim
- TrenchBroom: https://github.com/TrenchBroom/TrenchBroom
- rathammer: https://github.com/nmalthouse/rathammer
- Lambda Level Editor: https://developer.valvesoftware.com/wiki/Lambda_Level_Editor
