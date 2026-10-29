# Source map

A directory-by-directory guide to `src/`. Each section names the important files and says what
each one is for. Files not listed are usually helpers of the ones that are. A directory listing
will show them, and the header comment of each file says what it owns.

Every layer's dependencies are in [architecture.md](architecture.md#layers). Each layer is also
one CMake target, named `apex_<layer>`.

## `src/core/` (layer `core`)

Plain C++ with no third-party types in its interfaces.

| File | Purpose |
|---|---|
| `kv1.hpp`, `kv1.cpp`, `kv1_parse.cpp`, `kv1_write.cpp`, `kv1_detail.hpp` | The KV1 format (VMF and most Source text files). Tokenises and parses with byte spans and line numbers, and writes it back. |
| `kv3.hpp`, `kv3.cpp` | The KV3 format, used by newer Source data files. |
| `json.hpp`, `json.cpp` | JSON for settings and the on-disk caches. |
| `handle.hpp` | Handles that never revive after deletion, so stale references fail safely. |
| `job_pool.hpp`, `job_pool.cpp` | Background worker pool for index builds and parsing. |
| `log.hpp`, `log.cpp` | Logging. Use `apex::log` rather than `std::cerr`. |
| `paths.hpp`, `paths.cpp` | Path helpers. |
| `result.hpp` | The result type used across the layers. |
| `math.hpp` | Shared maths types and helpers. |
| `strings.hpp`, `strings.cpp` | String helpers. |
| `entity_class.hpp`, `entity_helpers.hpp`, `entity_helpers.cpp` | Entity class and key helpers. |
| `version.hpp`, `version.cpp` | Version queries, backed by the generated `apex/version.hpp`. |

## `src/geometry/` (layer `geometry`)

The brush kernel. Plane, side and solid maths with no document or editor knowledge.

| File | Purpose |
|---|---|
| `solid_geometry.hpp`, `.cpp` | Builds a solid's faces, vertices and edges from its planes. Reconciles `point_data` against the saved planes. |
| `solid_block.hpp`, `.cpp` | Builds a block solid from its bounds. |
| `convex_polyhedron.hpp`, `.cpp`, `convex_hull.cpp` | Convex hulls and polyhedra. The hull is fuzzed in CI. |
| `primitives.hpp`, `.cpp` | Primitive shapes for the primitive tool. |
| `face_uv.hpp`, `.cpp` | Texture axes, scale and offset for a face, and UV projection. |
| `face_matching.hpp`, `.cpp` | Matches faces between two versions of a solid. |
| `bvh.hpp`, `.cpp` | Bounding-volume hierarchy. |
| `tolerances.hpp` | Geometry tolerances. Use these rather than new local epsilons. |

## `src/document/` (layer `document`)

The map as data.

| File | Purpose |
|---|---|
| `document.hpp`, `.cpp` | The `Document`: node tree, IDs, transactions and history. Start here. |
| `vmf.hpp`, `.cpp` | The typed VMF model over KV1 nodes: world, entities, solids, sides, visgroups, groups. |
| `connection_codec.hpp`, `.cpp` | Parses and writes entity I/O connections (`connections` blocks). |
| `entity_transform.hpp`, `.cpp` | Moves, rotates and scales entities, including their origin and angles. |
| `instance_contents.hpp` | The contents of an instance, as the document holds them. |
| `instance_fixup.hpp`, `.cpp` | Fixes up instance contents when an instance is placed. |
| `instance_collapse.hpp`, `.cpp` | Collapses an instance back into its contents. |

## `src/editor/` (layer `editor`)

What the user does to the document. About 100 files; grouped here by job.

| Group | Key files |
|---|---|
| Session and commands | `editor_session.hpp/.cpp` (the owner), `actions.hpp/.cpp` (named actions and their defaults), `preset_io.hpp/.cpp` (keybinding presets on disk), `autosave.hpp/.cpp` |
| Tools | `tool.hpp/.cpp` (the interface), `select_tool.cpp`, `entity_tool.cpp`, `block_tool.hpp`, `clip_tool.cpp`, `draw_extrude_tool.cpp`, `primitive_tool.cpp`, `vertex_tool.cpp`, `measure_tool.cpp`, `apply_tool.cpp` |
| Selection | `selection.hpp/.cpp` (the selection value), `selection_ops.cpp`, `selection_expand.cpp`, `selection_undo.cpp`, `group_index.cpp` |
| Picking and snapping | `pick_index.hpp/.cpp`, `pick_model.hpp/.cpp`, `snapper.hpp/.cpp`, `work_plane.hpp/.cpp`, `placement.hpp/.cpp` |
| Transforms and handles | `transform.hpp/.cpp`, `modal_transform.hpp/.cpp`, `gizmo.hpp/.cpp`, `vertex_edit.cpp`, `face_edit.cpp`, `topology_ops.cpp` |
| Preview | `preview.hpp/.cpp` (ghosts and preview geometry) |
| Groups, visgroups, hiding | `group_ops.cpp`, `visgroups.hpp/.cpp`, `visibility.hpp/.cpp`, `hide_ops.cpp` |
| Clipboard | `clipboard.hpp/.cpp` |
| Entity I/O and instances | `connection_model.hpp/.cpp`, `connection_validation.cpp`, `connection_completion.cpp`, `incoming_connections.cpp`, `instance_templates.cpp`, `instance_expansion.cpp`, `instance_watch_state.cpp`, `instance_collapse_command.cpp`, `rename_propagation.cpp` |
| Inspector data | `inspector_model.hpp/.cpp`, `name_index.cpp` |
| Pie menus | `pie_menu.hpp/.cpp`, `pie_menus.hpp/.cpp` |

## `src/assets/` (layer `assets`)

Everything that reads the game's files. Third-party format libraries stay behind this layer.

| File | Purpose |
|---|---|
| `vfs.hpp`, `.cpp` | Virtual filesystem: mounts, Steam discovery, VPK reads. Earlier mounts win. Main-thread owner. |
| `game_profile.hpp`, `.cpp` | A game's `gameinfo`, mounts and search paths. P2CE/Strata is the only profile planned through 0.10. |
| `addons.hpp`, `.cpp` | Strata addon mounting. Background and open questions are in [../plans/addons.md](../plans/addons.md). |
| `vmt.hpp`, `.cpp` | Material (VMT) parsing. |
| `vtf.hpp`, `.cpp` | Texture (VTF) decoding. |
| `fgd.hpp`, `.cpp` | Entity definitions (FGD), which drive the inspector. |
| `material_index.hpp`, `.cpp`, `material_cache.hpp`, `.cpp` | Material index and its on-disk cache. |
| `model_index.hpp`, `model_data.hpp`, `model_cache.hpp`, `.cpp` | Model index, model data, and the model cache. |
| `model_bake.hpp`, `.cpp`, `model_pose.hpp`, `.cpp` | Model baking and poses. Apex's own bake; the upstream one is forbidden. |
| `sound_index.hpp`, `particle_index.hpp` | Sound and particle indexes. |
| `asset_index.hpp`, `.cpp` | The general asset index that the browsers share. |
| `thumbnail_cache.hpp`, `.cpp` | Thumbnail cache on disk. |
| `path_expand.hpp` | Path expansion helpers. |

## `src/render/` (layer `render`)

Vulkan. The GPU code lives here. `ui/gpu_host` is the bridge that connects it to Qt.

| File | Purpose |
|---|---|
| `context.hpp`, `.cpp` | Instance, device, queue and allocator. One per process. |
| `vk.hpp`, `vk.cpp`, `vma_impl.cpp` | Vulkan loader and allocator glue. |
| `renderer.hpp`, `.cpp` | Pipelines, draw recording, reversed-Z, camera-relative drawing. |
| `resources.hpp`, `.cpp`, `render_target.hpp`, `.cpp`, `swapchain.hpp`, `.cpp` | GPU buffers, images, render targets and the swapchain. |
| `texture_heap.hpp`, `.cpp`, `upload_ring.hpp`, `.cpp` | Bindless texture heap and the staged-upload ring. |
| `solid_mesh.hpp`, `.cpp`, `helper_shapes.hpp`, `.cpp` | Mesh building for solids and helper shapes (grids, gizmos). |
| `glyph_atlas.hpp`, `.cpp` | Font atlas for labels. |
| `render_mirror.hpp`, `.cpp` | Render mirroring. Read the header for its contract. |
| `model_thumbnail.hpp`, `.cpp`, `model_thumbnail_scheduler.hpp`, `.cpp` | Renders model thumbnails in the background. |
| `model_inspector.hpp`, `.cpp` | Loads and renders one model for the Models browser's inspect view. |
| `view_math.hpp` | Projection and camera-relative maths. |
| `shaders/` | GLSL sources. Compiled at build time; see [repo-map.md](repo-map.md#where-generated-code-comes-from). |
| `shader_blobs.hpp`, `.cpp` | Access to the compiled shader blobs. |

## `src/viewport/` (layer `viewport`)

How the map is seen. No Qt here.

| File | Purpose |
|---|---|
| `viewport.hpp`, `.cpp` | A viewport: camera, display mode, overlays. |
| `camera.hpp`, `.cpp` | Perspective and orthographic cameras, free and axis-locked. |
| `controllers.hpp`, `.cpp` | Turn input into camera moves. |
| `layout.hpp`, `.cpp` | The split tree of areas. |
| `workspace.hpp`, `.cpp` | Named workspaces, each holding a whole layout. Built-in ones are "Layout", "Modeling" and "Four Views". |
| `map_views.hpp`, `.cpp` | Saved per-map views, stored in the VMF's `viewsettings` block. |
| `pick_math.hpp`, `.cpp` | Turns a pixel and a viewport into a ray. |
| `view_context.hpp`, `.cpp` | What a tool sees of a viewport: plain camera data and the picking callback. |

## `src/ui/` (layer `ui`)

Qt. The largest directory after `editor`, and the only one that includes Qt headers besides `app`.

| Group | Key files |
|---|---|
| Shell | `main_window.hpp/.cpp` (the window), `area_frame.cpp`, `workspace_bar.cpp`, `theme.hpp/.cpp`, `theme_editor.cpp`, `settings_dialog.cpp` |
| Viewports | `viewport_area.hpp/.cpp`, `viewport_pane.hpp/.cpp`, `gpu_host.hpp/.cpp` (puts Vulkan into a Qt window), `uv_view.cpp` |
| Panels | `outliner_panel.cpp`, `inspector_panel.cpp`, `history_panel.cpp`, `log_panel.cpp`, `visgroup_panel.cpp`, `face_properties_panel.cpp`, `tool_parameter_strip.cpp` |
| Asset browsers | `asset_browser_panel.cpp`, `materials_panel.cpp`, `models_panel.cpp`, `prefab_thumbnail.cpp`, `model_inspect_view.hpp/.cpp`, `model_pick.hpp/.cpp`, `sounds_panel.cpp`, `sound_preview.cpp`, `particles_panel.cpp`, `game_content.hpp/.cpp` |
| Input | `key_chord.hpp/.cpp`, `pie_menu_popup.cpp`, `pointer_lock.hpp/.cpp`, `pointer_lock_wayland.cpp` (Wayland-only), `input_log.hpp/.cpp`, `transform_input.cpp` |
| Dialogs | `paste_special_dialog.cpp`, `transform_dialog.cpp`, `tie_dialog.cpp`, `prefab_dialog.cpp`, `recovery_dialog.cpp`, `rename_confirmation.cpp` |
| Icons | `icons/` and `icons.qrc`, compiled into the executable |

## `src/app/` (layer `app`)

| File | Purpose |
|---|---|
| `main.cpp` | `main()`. Starts Qt, logging and themes, creates `MainWindow`, and opens a map if one is given on the command line. |


## Compile and play (0.5)

`src/compile/` depends on core only: profiles, command expansion, file operations, the sequence
state machine, detached game arguments, diagnostics and leak/portal readers. `src/editor/compile_snapshot.*`
writes isolated snapshots and copies/rebases local instances. `src/editor/instance_create.cpp` owns
Create Instance from Selection, its crossing-connection warning and the single document transaction.

`src/assets/addon_authoring.*` owns the addon scaffold, campaign lookup and ordered mount comparison;
`Vfs::compileGameInfo` supplies compile-tool content. `src/ui/compile_panel.*`, `profile_editor.*`,
`profile_store.*`, `qprocess_runner.*` and `addons_panel.*` provide the Qt controls and native runner.
