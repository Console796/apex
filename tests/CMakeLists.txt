include(doctest)

add_executable(apex_tests main.cpp core/version_test.cpp core/math_test.cpp core/handle_test.cpp
               core/result_test.cpp core/strings_test.cpp core/paths_test.cpp
               core/log_test.cpp core/job_pool_test.cpp core/kv1_test.cpp
               document/vmf_test.cpp document/document_test.cpp document/p0_write_path_test.cpp
               document/geometry_number_test.cpp document/solid_geometry_test.cpp
               assets/vfs_test.cpp assets/game_profile_test.cpp assets/material_test.cpp
               assets/texture_test.cpp assets/fgd_test.cpp assets/material_cache_test.cpp
               assets/material_corpus_test.cpp core/json_test.cpp
               geometry/corpus_test.cpp geometry/convex_polyhedron_test.cpp
               geometry/face_matching_test.cpp
               geometry/solid_geometry_test.cpp geometry/bvh_test.cpp
               editor/pick_index_test.cpp editor/visibility_test.cpp editor/hide_ops_test.cpp editor/actions_test.cpp
               editor/group_index_test.cpp editor/editor_session_test.cpp editor/selection_test.cpp
               editor/selection_expand_test.cpp editor/selection_corpus_test.cpp editor/work_plane_test.cpp
               editor/transform_test.cpp editor/preview_test.cpp
               editor/gizmo_test.cpp editor/modal_transform_test.cpp editor/session_transform_test.cpp editor/clipboard_test.cpp
               render/view_math_test.cpp render/context_test.cpp render/renderer_test.cpp render/render_mirror_test.cpp
               render/p0_point_data_test.cpp
               viewport/pick_math_test.cpp viewport/viewport_test.cpp viewport/layout_test.cpp viewport/map_views_test.cpp)
target_link_libraries(apex_tests PRIVATE apex_viewport apex_render apex_assets sourcepp::vpkpp sourcepp::steampp
                                         sourcepp::vtfpp sourcepp::toolpp
                                         apex_editor apex_document apex_geometry apex_core apex_warnings
                                         doctest::doctest)
doctest_discover_tests(apex_tests)

target_compile_definitions(apex_tests PRIVATE APEX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
                                              APEX_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/render/golden")
