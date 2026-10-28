# Find by task

Start from what you want to change. Each row names the files to read first. Grep for a symbol
name from the header to find its uses, and read the header comment of each file before editing it.

| I want to... | Start with | Then look at |
|---|---|---|
| Understand the design rules before a change | [architecture.md](architecture.md) | [`docs/plans/0.1.md`](../plans/0.1.md) section 2 |
| Add an editor tool | [`src/editor/tool.hpp`](../../src/editor/tool.hpp) | an existing tool such as `select_tool.cpp`, and `editor_session.hpp`. Grep the tool class name to see how it is wired in. |
| Change keybindings or add an action | [`src/editor/actions.hpp`](../../src/editor/actions.hpp) | `preset_io.hpp`, `ui/key_chord.cpp`, `ui/settings_dialog.cpp` |
| Change pie menus | [`src/editor/pie_menus.hpp`](../../src/editor/pie_menus.hpp) | `pie_menu.hpp`, `ui/pie_menu_popup.cpp`, `tests/editor/pie_menu_test.cpp` |
| Change how a VMF is read or written | [`src/core/kv1.hpp`](../../src/core/kv1.hpp) | `kv1_parse.cpp`, `kv1_write.cpp`, `src/document/vmf.hpp` |
| Change the document model or undo | [`src/document/document.hpp`](../../src/document/document.hpp) | `editor/selection_undo.cpp`, `ui/history_panel.cpp` |
| Change brush geometry | [`src/geometry/solid_geometry.hpp`](../../src/geometry/solid_geometry.hpp) | `solid_block.cpp`, `face_uv.cpp`, `tests/geometry/` |
| Change how faces are textured or UV-mapped | [`src/geometry/face_uv.hpp`](../../src/geometry/face_uv.hpp) | `editor/face_edit.cpp`, `ui/face_properties_panel.cpp` |
| Change entity properties in the inspector | [`src/editor/inspector_model.hpp`](../../src/editor/inspector_model.hpp) | `assets/fgd.hpp` (the definitions), `ui/inspector_panel.cpp` |
| Change entity I/O connections | [`src/document/connection_codec.hpp`](../../src/document/connection_codec.hpp) | `editor/connection_model.hpp`, `connection_validation.cpp`, `ui/` |
| Change instances or prefabs | [`src/document/instance_fixup.hpp`](../../src/document/instance_fixup.hpp) | `instance_collapse.hpp`, `editor/instance_expansion.cpp`, `editor/instance_templates.cpp`, `tools/instance_collapse/` |
| Change the materials browser | [`src/assets/material_index.hpp`](../../src/assets/material_index.hpp) | `material_cache.hpp`, `vmt.hpp`, `vtf.hpp`, `ui/materials_panel.cpp` |
| Change the models browser or model thumbnails | [`src/assets/model_index.hpp`](../../src/assets/model_index.hpp) | `assets/model_bake.hpp`, `render/model_thumbnail.hpp`, `render/model_inspector.hpp`, `ui/models_panel.cpp`, `ui/model_inspect_view.hpp` |
| Change the sounds or particles browsers | [`src/assets/sound_index.hpp`](../../src/assets/sound_index.hpp), [`particle_index.hpp`](../../src/assets/particle_index.hpp) | `ui/sounds_panel.cpp`, `ui/sound_preview.cpp`, `ui/particles_panel.cpp` |
| Change how the game's files are found | [`src/assets/game_profile.hpp`](../../src/assets/game_profile.hpp) | `assets/vfs.hpp`, `assets/addons.hpp` |
| Change the renderer or a shader | [`src/render/renderer.hpp`](../../src/render/renderer.hpp) | `render/shaders/`, [`cmake/Shaders.cmake`](../../cmake/Shaders.cmake), `render/solid_mesh.hpp` |
| Change camera controls or the view layout | [`src/viewport/viewport.hpp`](../../src/viewport/viewport.hpp) | `viewport/controllers.hpp`, `viewport/layout.hpp`, `viewport/workspace.hpp` |
| Change how a click becomes a ray | [`src/viewport/pick_math.hpp`](../../src/viewport/pick_math.hpp) | `editor/pick_index.hpp`, `editor/snapper.hpp` |
| Change snapping | [`src/editor/snapper.hpp`](../../src/editor/snapper.hpp) | `editor/work_plane.hpp`, `editor/placement.hpp` |
| Change gizmos or modal transforms | [`src/editor/gizmo.hpp`](../../src/editor/gizmo.hpp), [`modal_transform.hpp`](../../src/editor/modal_transform.hpp) | `editor/transform.hpp`, `ui/transform_input.cpp` |
| Change selection behaviour | [`src/editor/selection.hpp`](../../src/editor/selection.hpp) | `selection_ops.cpp`, `selection_expand.cpp`, `group_index.cpp` |
| Change groups, visgroups or hiding | `src/editor/group_ops.cpp`, [`visgroups.hpp`](../../src/editor/visgroups.hpp) | `visibility.hpp`, `hide_ops.cpp`, `ui/visgroup_panel.cpp` |
| Change copy and paste | [`src/editor/clipboard.hpp`](../../src/editor/clipboard.hpp) | `ui/paste_special_dialog.cpp` |
| Change autosave or crash recovery | [`src/editor/autosave.hpp`](../../src/editor/autosave.hpp) | `ui/recovery_dialog.cpp` |
| Change panels or the main window | [`src/ui/main_window.hpp`](../../src/ui/main_window.hpp) | `ui/area_frame.cpp`, `ui/workspace_bar.cpp`, the panel you need |
| Change themes or colours | [`src/ui/theme.hpp`](../../src/ui/theme.hpp) | `ui/theme_editor.cpp`, `ui/settings_dialog.cpp` |
| Change the Wayland pointer handling | [`src/ui/pointer_lock.hpp`](../../src/ui/pointer_lock.hpp) | `ui/pointer_lock_wayland.cpp`, `third_party/wayland-protocols/` |
| Change logging | [`src/core/log.hpp`](../../src/core/log.hpp) | the callers, with grep |
| Run work on a background thread | [`src/core/job_pool.hpp`](../../src/core/job_pool.hpp) | `assets/asset_index.hpp` for the owner-thread rules |
| Add a source file or a dependency | [`CMakeLists.txt`](../../CMakeLists.txt) (the `apex_layer` calls) | [`cmake/Layers.cmake`](../../cmake/Layers.cmake), [`cmake/deps.cmake`](../../cmake/deps.cmake), [`THIRD_PARTY.md`](../../THIRD_PARTY.md), `tools/check_licenses.py` |
| Change a layer's allowed includes | `DEPS` in [`tools/check_layers.py`](../../tools/check_layers.py) | [architecture.md](architecture.md#layers) |
| Add a test | the matching folder in [`tests/`](../../tests/) | `tests/CMakeLists.txt` (tests are listed there) |
| Add a fixture | [`tests/fixtures/README.md`](../../tests/fixtures/README.md) | the `Apex-authored` marker rule |
| Change the CI gate | [`tools/ci.py`](../../tools/ci.py) | [../development.md](../development.md) section 5 |
| Package a release | [`tools/package.py`](../../tools/package.py) | [`cmake/Packaging.cmake`](../../cmake/Packaging.cmake), [`CONTRIBUTING.md`](../../CONTRIBUTING.md) |
| Check what a milestone should contain | [`docs/roadmap.md`](../roadmap.md) | the plan for that milestone in [`docs/plans/`](../plans/) |

## Before you commit

1. `python3 tools/check_layers.py`
2. `git diff --check`
3. The tests that cover your change (see [tests-tools-docs.md](tests-tools-docs.md#running-tests)).
4. Sign off the commit with `git commit -s`, as [CONTRIBUTING.md](../../CONTRIBUTING.md) requires.
