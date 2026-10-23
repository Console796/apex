include(doctest)

add_executable(apex_tests main.cpp core/version_test.cpp core/math_test.cpp core/handle_test.cpp
               core/result_test.cpp core/strings_test.cpp core/paths_test.cpp
               core/log_test.cpp core/job_pool_test.cpp core/kv1_test.cpp
               document/vmf_test.cpp document/document_test.cpp document/p0_write_path_test.cpp document/p0_topology_test.cpp
               document/geometry_number_test.cpp document/solid_geometry_test.cpp document/rebuild_solid_test.cpp
               assets/vfs_test.cpp assets/vfs_walk_test.cpp assets/game_profile_test.cpp assets/material_test.cpp
               assets/texture_test.cpp assets/fgd_test.cpp assets/material_cache_test.cpp
               assets/material_corpus_test.cpp core/json_test.cpp
               geometry/corpus_test.cpp geometry/convex_polyhedron_test.cpp
               geometry/face_matching_test.cpp geometry/kernel_test.cpp
               geometry/solid_geometry_test.cpp geometry/bvh_test.cpp
               editor/autosave_test.cpp editor/inspector_model_test.cpp editor/placement_test.cpp editor/tool_test.cpp editor/tool_dispatch_test.cpp editor/block_tool_test.cpp
               editor/snapper_test.cpp editor/pick_index_test.cpp editor/visibility_test.cpp editor/hide_ops_test.cpp editor/visgroups_test.cpp editor/actions_test.cpp editor/preset_io_test.cpp
               editor/group_index_test.cpp editor/editor_session_test.cpp editor/selection_test.cpp
               editor/selection_expand_test.cpp editor/selection_corpus_test.cpp editor/rearrange_corpus_test.cpp editor/hide_corpus_test.cpp editor/history_property_test.cpp editor/work_plane_test.cpp
               editor/transform_test.cpp editor/preview_test.cpp editor/preview_geometry_test.cpp
               editor/gizmo_test.cpp editor/modal_transform_test.cpp editor/session_transform_test.cpp editor/clipboard_test.cpp editor/clipboard_corpus_test.cpp
               render/view_math_test.cpp render/context_test.cpp render/renderer_test.cpp render/render_mirror_test.cpp
               render/p0_point_data_test.cpp render/preview_overlay_test.cpp
               viewport/pick_math_test.cpp viewport/viewport_test.cpp viewport/layout_test.cpp viewport/map_views_test.cpp viewport/workspace_test.cpp)
target_link_libraries(apex_tests PRIVATE apex_viewport apex_render apex_assets sourcepp::vpkpp sourcepp::steampp
                                         sourcepp::vtfpp sourcepp::toolpp
                                         apex_editor apex_document apex_geometry apex_core apex_warnings
                                         doctest::doctest)
doctest_discover_tests(apex_tests)

# Keep the opt-in measurement workload separately tagged and visibly skipped in ctest.
add_executable(apex_measure_tests main.cpp editor/measure_test.cpp editor/p0_corpus_counts_test.cpp)
target_link_libraries(apex_measure_tests PRIVATE apex_editor apex_assets sourcepp::vpkpp sourcepp::steampp apex_warnings doctest::doctest)
doctest_discover_tests(apex_measure_tests TEST_PREFIX "measure::"
                      PROPERTIES LABELS "measure" SKIP_REGULAR_EXPRESSION "APEX_MEASURE=1 is required")

target_compile_definitions(apex_tests PRIVATE APEX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
                                              APEX_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/render/golden")
