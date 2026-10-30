# Glossary

Terms you will meet in the code, the docs and the commit history. Source-engine terms come first,
then Strata and the project's own vocabulary.

## Source engine and file formats

| Term | Meaning |
|---|---|
| **Source** | Valve's game engine. Apex reads and writes its map format. |
| **Strata Source** | The Strata engine fork that Apex targets. Maps follow its conventions, which differ from Valve's in places. |
| **P2CE** | Portal 2 Community Edition, built on Strata. The test corpus and the only game profile come from it. |
| **Strata Hammer** | The level editor for Strata maps. Apex must round-trip files with it without loss. |
| **VMF** | The map file format: a KV1 document holding the world, entities, solids and view settings. |
| **KV1** | Valve's key-value text format (`"key" "value"` with blocks). Parsed in `src/core/kv1*`. |
| **KV3** | Valve's newer data format, parsed in `src/core/kv3*`. |
| **VMT** | Material definition: shader and parameters, in KV1. |
| **VTF** | Valve texture format. Decoded by `src/assets/vtf.cpp`. |
| **VPK** | Valve package archive. Read through the VFS. |
| **FGD** | Entity definition file. It describes each entity class's properties and I/O, and drives the inspector. |
| **gameinfo** | The game's configuration file, listing its search paths. Read by the game profile. |
| **MDL** | Compiled model format. Baked by Apex in `src/assets/model_bake.cpp`. |
| **Addon** | A Strata addon: a set of files mounted into a game profile. See [../plans/addons.md](../plans/addons.md). |

## Map structure

| Term | Meaning |
|---|---|
| **World** | The `world` block of a VMF: the solids that make up the static map. |
| **Solid** | A convex brush, made of sides. The unit of brush geometry. |
| **Side** | One face of a solid: a plane, a material, and texture axes. Sides have their own IDs. |
| **Plane** | The equation that defines a side. A solid's geometry comes from its planes. |
| **Point entity** | An entity with an origin and no solids, such as a light. |
| **Brush entity** | An entity made of solids, such as a door or a trigger. |
| **Entity I/O** | The `connections` block: outputs that fire inputs on other entities. |
| **Group** | A set of solids and entities that is edited as one unit. Stored flat, with a `groupid` on each member. |
| **Instance** | A prefab: a map file placed as an entity and expanded into the map. Handled by `document/instance_*` and `editor/instance_*`. |
| **Visgroup** | A named set of objects that can be shown or hidden together. |
| **Hidden** | Objects hidden with H, which Hammer wraps in a `hidden` block. |
| **Displacement** | A subdivided, deformable face. Planned for 0.8. |
| **Texture lock** | Keeps texture alignment fixed while a brush is transformed. |
| **point_data** | Per-solid data that Apex reconciles against the planes (`solid_geometry`, decision D8). |
| **Cordon** | A box in the `cordons` block that limits a compile to part of the map. Cordons were added in 0.5. |
| **light_rt** | Strata's dynamic light entity. The 0.6 preview shades it, and its direct, indirect and specular modes are planned. |
| **Light model** | The shared description of every light, built once from entity keys in `src/lighting/`. The preview uses it now, and the baker will use it in 0.7. |

## Apex terms

| Term | Meaning |
|---|---|
| **Document** | The in-memory map: a tree of nodes with stable IDs and a transaction history. |
| **Compile profile** | A saved set of compile steps and their variables, kept in the user's preferences, not in the map. |
| **Sequence** | An ordered list of compile tool runs, such as Fast or Final. Each step has a status: queued, running, ok, failed, cancelled or skipped. |
| **Diagnostic** | A problem parsed from a compile log, with a stable ID. Diagnostics link back to the object that caused them. |
| **Problem** | An entry in the Problems list. It selects an object and never changes the map by itself (0.5 D8). |
| **Addon target** | An addon that a compile writes its maps into, rather than the game's own `maps` folder (0.5.7). |
| **Transaction** | One logged change to the document. Undo, redo and the history panel work from these. |
| **Handle** | An ID for a node. It never revives once its node is deleted. |
| **Codec** | The part of the document that keeps unknown keys, spans and original spelling, so unchanged data is written back exactly. |
| **Editor session** | The owner of the document plus unsaved state: selection, the active tool, the pick index. |
| **Tool** | An object that turns pointer and key events into edits. Implements `tool.hpp`. |
| **Selection** | The objects the user has picked. Session state, never written to the file. |
| **Modal transform** | A move, rotate or scale started from the keyboard, following the pointer until it is confirmed. |
| **Gizmo** | The on-screen handle for a transform. |
| **Snapper** | Snaps a point to vertices, edges or the grid, in that priority order. |
| **Work plane** | The plane that placement and drags are measured on. See `editor/work_plane.hpp`. |
| **Pie menu** | A radial menu opened from a key or the pointer. |
| **Viewport** | A camera with a display mode and overlays. Perspective, or orthographic 2D. |
| **Layout** | A split tree of viewport and panel areas. |
| **Workspace** | A named layout that the user switches between, as in Blender. |
| **Game profile** | The description of one game: its gameinfo, mounts and addons. |
| **Mount** | A source of files in the VFS. Earlier mounts win. |
| **VFS** | The virtual filesystem that presents mounts, VPKs and the loose files as one tree. |
| **Index** | A background-built list of assets (materials, models, sounds, particles) for the browsers. |
| **Corpus** | The real P2CE maps used by the corpus tests. Not in the repository. |
| **Golden image** | A reference render that a render test compares against. Stored in `tests/render/golden/`. |
| **Fixture** | A test input that Apex wrote. Must carry the `Apex-authored` marker. |
| **Probe** | A small experiment that settles an unknown before real work starts. Its output is in `docs/plans/probe-results/`. |
| **Decision (D<n>)** | A numbered design decision in a plan. Code comments cite them. |
| **Milestone, sub-milestone** | The roadmap's units of work, for example 0.4 and 0.4.6. See [../roadmap.md](../roadmap.md). |
| **Release tag** | A version ending in `-release`, in `version.txt`. |
| **CI gate** | `tools/ci.py`: the local run of every check, which replaces hosted CI. |

## Graphics

| Term | Meaning |
|---|---|
| **Vulkan** | The graphics API the renderer uses. The target is Vulkan 1.3. |
| **Reversed-Z** | A depth scheme that stores depth with 0 as the far plane, to keep precision. |
| **Camera-relative** | Positions are sent to the GPU relative to the camera, so large coordinates keep their precision. |
| **Bindless texture heap** | One descriptor set that holds every texture, indexed from the shader. |
| **Lit** | The display mode that shows the map with direct light and sky ambient, rendered in HDR and tone-mapped with a fixed exposure from the map (0.6). |
| **SPIR-V** | The compiled form of the shaders, embedded in the binary. |
| **lavapipe** | Mesa's software Vulkan driver. The required device for GPU tests. |
| **RADV** | Mesa's AMD driver. Used for GPU tests when an AMD GPU is present. |
| **VMA** | The Vulkan Memory Allocator library, used for GPU allocations. |
