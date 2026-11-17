include(doctest)

add_executable(apex_chart_reference lighting/chart_reference.cpp)
target_link_libraries(apex_chart_reference PRIVATE apex_lighting sourcepp::bsppp apex_warnings)

add_executable(apex_tests main.cpp compile/compile_test.cpp core/version_test.cpp core/math_test.cpp core/handle_test.cpp
               core/entity_helpers_test.cpp core/result_test.cpp core/strings_test.cpp core/paths_test.cpp
               core/log_test.cpp core/job_pool_test.cpp core/kv1_test.cpp core/kv3_test.cpp
               document/instance_test.cpp document/sidelist_remap_test.cpp document/connection_codec_test.cpp document/vmf_test.cpp document/document_test.cpp document/p0_write_path_test.cpp document/p0_topology_test.cpp
               document/entity_transform_test.cpp document/geometry_number_test.cpp document/solid_geometry_test.cpp document/rebuild_solid_test.cpp document/displacement_test.cpp
               document/sidelist_upkeep_test.cpp document/overlay_oracle_test.cpp document/detail_props_oracle_test.cpp
               assets/vfs_test.cpp assets/addons_test.cpp assets/addon_authoring_test.cpp assets/addons_corpus_test.cpp assets/vfs_walk_test.cpp assets/game_profile_test.cpp assets/material_test.cpp
               assets/texture_test.cpp assets/hotspot_test.cpp assets/fgd_test.cpp assets/material_cache_test.cpp
               assets/model_bake_test.cpp assets/model_bake_corpus_test.cpp assets/model_cache_test.cpp assets/sourcepp_confirm_test.cpp
               assets/material_corpus_test.cpp assets/material_index_test.cpp assets/asset_index_test.cpp assets/asset_index_corpus_test.cpp assets/thumbnail_test.cpp assets/external_thumbnail_test.cpp core/json_test.cpp
               lighting/light_test.cpp lighting/bsp_lightmaps_test.cpp lighting/sky_visibility_test.cpp lighting/specular_test.cpp lighting/charting_test.cpp lighting/cpu_accel_test.cpp lighting/baker_test.cpp lighting/bake_session_test.cpp lighting/bake_diff_test.cpp lighting/bake_casters_test.cpp lighting/displacement_lightmap_test.cpp
               geometry/corpus_test.cpp geometry/convex_polyhedron_test.cpp
               geometry/face_uv_test.cpp geometry/face_matching_test.cpp geometry/kernel_test.cpp geometry/primitives_test.cpp
               geometry/solid_geometry_test.cpp geometry/bvh_test.cpp geometry/displacement_test.cpp geometry/displacement_ops_test.cpp geometry/displacement_sculpt_test.cpp geometry/hotspot_test.cpp geometry/rope_test.cpp
               geometry/overlay_test.cpp
editor/cordon_test.cpp editor/compile_snapshot_test.cpp editor/vrad_roundtrip_test.cpp editor/diagnostic_lookup_test.cpp editor/autosave_test.cpp editor/instance_collapse_command_test.cpp editor/instance_create_test.cpp editor/inspector_model_test.cpp editor/placement_test.cpp editor/placement_item_test.cpp editor/tool_test.cpp editor/tool_dispatch_test.cpp editor/block_tool_test.cpp editor/chamber_test.cpp editor/primitive_tool_test.cpp editor/clip_tool_test.cpp editor/face_edit_test.cpp editor/apply_tool_test.cpp editor/hotspot_edit_test.cpp editor/measure_tool_test.cpp editor/overlay_tool_test.cpp editor/rope_tool_test.cpp editor/topology_ops_test.cpp editor/vertex_edit_test.cpp editor/vertex_tool_test.cpp
               editor/pie_menu_test.cpp
               editor/displacement_pick_test.cpp editor/displacement_topology_test.cpp editor/displacement_tool_test.cpp editor/displacement_sculpt_test.cpp
               editor/snapper_test.cpp editor/pick_index_test.cpp editor/pick_model_test.cpp editor/visibility_test.cpp editor/hide_ops_test.cpp editor/visgroups_test.cpp editor/actions_test.cpp editor/preset_io_test.cpp
               editor/group_index_test.cpp editor/name_index_test.cpp editor/connection_model_test.cpp editor/incoming_connections_test.cpp editor/rename_propagation_test.cpp editor/connection_validation_test.cpp editor/instance_templates_test.cpp editor/instance_expansion_test.cpp editor/instance_watch_state_test.cpp assets/connection_corpus_test.cpp editor/editor_session_test.cpp editor/selection_test.cpp
               editor/selection_expand_test.cpp editor/selection_corpus_test.cpp editor/rearrange_corpus_test.cpp editor/hide_corpus_test.cpp editor/history_property_test.cpp editor/work_plane_test.cpp
               editor/transform_test.cpp editor/displacement_transform_test.cpp editor/vector_key_test.cpp editor/preview_test.cpp editor/preview_geometry_test.cpp
               editor/gizmo_test.cpp editor/modal_transform_test.cpp editor/session_transform_test.cpp editor/clipboard_test.cpp editor/sidelist_paste_test.cpp editor/clipboard_corpus_test.cpp editor/prefab_test.cpp editor/asset_report_test.cpp editor/map_problems_test.cpp editor/entity_report_test.cpp editor/find_replace_test.cpp
               render/lightmap_atlas_test.cpp render/vrad_real_tools_test.cpp render/instance_lightmap_test.cpp render/cluster_test.cpp render/shadow_test.cpp render/view_math_test.cpp render/helper_shapes_test.cpp render/context_test.cpp render/rayquery_test.cpp render/gpu_bake_test.cpp render/scene_accel_test.cpp render/probe_volume_test.cpp render/renderer_test.cpp render/scene_target_test.cpp render/render_mirror_test.cpp
               ../src/ui/model_pick.cpp render/model_render_test.cpp render/model_thumbnail_test.cpp render/p0_point_data_test.cpp render/preview_overlay_test.cpp
               viewport/pick_math_test.cpp viewport/viewport_test.cpp viewport/layout_test.cpp viewport/map_views_test.cpp viewport/workspace_test.cpp viewport/right_gate_test.cpp)
target_link_libraries(apex_tests PRIVATE apex_compile apex_viewport apex_render apex_assets sourcepp::vpkpp sourcepp::steampp
                                         sourcepp::vtfpp sourcepp::toolpp sourcepp::mdlpp
                                         apex_editor apex_lighting apex_document apex_geometry apex_core apex_warnings
                                         doctest::doctest)
apex_compile_shaders(TARGET apex_tests DIR "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders"
                     HEADER render/test_shaders_spv.hpp
                     INCLUDES "${PROJECT_SOURCE_DIR}/src/render/shaders/lighting.inc"
                              "${PROJECT_SOURCE_DIR}/src/render/shaders/probes.inc"
                              "${PROJECT_SOURCE_DIR}/src/render/shaders/frame.inc"
                              "${PROJECT_SOURCE_DIR}/src/render/shaders/normal_map.inc"
                              "${PROJECT_SOURCE_DIR}/src/render/shaders/specular.inc"
                              "${PROJECT_SOURCE_DIR}/src/render/shaders/material.inc")
doctest_discover_tests(apex_tests
                      PROPERTIES LABELS "apex-tests" ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")

# Keep the opt-in measurement workload separately tagged and visibly skipped in ctest.
add_executable(apex_measure_tests main.cpp editor/measure_test.cpp editor/displacement_sculpt_measure_test.cpp editor/overlay_drag_measure_test.cpp editor/p0_corpus_counts_test.cpp render/rayquery_measure_test.cpp render/scene_accel_measure_test.cpp render/bake_incremental_measure_test.cpp)
target_link_libraries(apex_measure_tests PRIVATE apex_render apex_editor apex_assets sourcepp::vpkpp sourcepp::steampp apex_warnings doctest::doctest)
doctest_discover_tests(apex_measure_tests TEST_PREFIX "measure::"
                      PROPERTIES LABELS "measure" SKIP_REGULAR_EXPRESSION "APEX_MEASURE=1 is required")

target_compile_definitions(apex_tests PRIVATE APEX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
                                              APEX_COMPILE_LOG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/compile/logs"
                                              APEX_PRIMITIVE_VMF_OUT="${CMAKE_BINARY_DIR}/primitives.vmf"
                                              APEX_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/render/golden")

add_executable(apex_plugin_host_tests main.cpp plugin_host/plugin_host_test.cpp)
add_library(apex_crash_plugin MODULE plugin_host/crash_plugin.c)
target_include_directories(apex_crash_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_executable(apex_plugin_crash_child plugin_host/crash_child.cpp)
target_link_libraries(apex_plugin_crash_child PRIVATE apex_plugin_host)
add_library(apex_read_plugin MODULE plugin_host/read_plugin.c)
target_include_directories(apex_read_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_library(apex_registration_plugin MODULE plugin_host/registration_plugin.c)
target_include_directories(apex_registration_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_library(apex_exception_plugin MODULE plugin_host/exception_plugin.cpp)
target_include_directories(apex_exception_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_dependencies(apex_plugin_host_tests apex_exception_plugin)
target_compile_definitions(apex_plugin_host_tests PRIVATE EXCEPTION_PLUGIN_PATH="$<TARGET_FILE:apex_exception_plugin>")
add_library(apex_bad_plugin MODULE plugin_host/bad_plugin.c)
target_include_directories(apex_bad_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_library(apex_write_plugin MODULE plugin_host/write_plugin.c)
target_include_directories(apex_write_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_library(apex_ui_plugin MODULE plugin_host/ui_plugin.c)
target_include_directories(apex_ui_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_dependencies(apex_plugin_host_tests apex_ui_plugin)
if(TARGET apex_ui_tests)
  add_dependencies(apex_ui_tests apex_ui_plugin)
  target_compile_definitions(apex_ui_tests PRIVATE UI_PLUGIN_PATH="$<TARGET_FILE:apex_ui_plugin>")
endif()
target_compile_definitions(apex_plugin_host_tests PRIVATE UI_PLUGIN_PATH="$<TARGET_FILE:apex_ui_plugin>")
target_link_libraries(apex_plugin_host_tests PRIVATE apex_plugin_host apex_warnings doctest::doctest)
add_dependencies(apex_plugin_host_tests apex_bad_plugin hello_tool apex_crash_plugin apex_plugin_crash_child apex_read_plugin apex_registration_plugin polygon_tool face_stats)
add_dependencies(apex_plugin_host_tests apex_write_plugin)
target_compile_definitions(apex_plugin_host_tests PRIVATE WRITE_PLUGIN_PATH="$<TARGET_FILE:apex_write_plugin>")
target_compile_definitions(apex_plugin_host_tests PRIVATE BAD_PLUGIN_PATH="$<TARGET_FILE:apex_bad_plugin>" HELLO_PLUGIN_PATH="$<TARGET_FILE:hello_tool>" CRASH_PLUGIN_PATH="$<TARGET_FILE:apex_crash_plugin>" CRASH_CHILD_PATH="$<TARGET_FILE:apex_plugin_crash_child>" READ_PLUGIN_PATH="$<TARGET_FILE:apex_read_plugin>" REGISTRATION_PLUGIN_PATH="$<TARGET_FILE:apex_registration_plugin>" POLYGON_PLUGIN_PATH="$<TARGET_FILE:polygon_tool>" FACE_STATS_PLUGIN_PATH="$<TARGET_FILE:face_stats>")
doctest_discover_tests(apex_plugin_host_tests TEST_PREFIX "plugin_host::")

# P1: compile each side independently with the other compiler, then load and call both functions.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_program(APEX_PROBE_GCC gcc)
  find_program(APEX_PROBE_GXX g++)
  find_program(APEX_PROBE_CLANG clang)
  find_program(APEX_PROBE_CLANGXX clang++)
  if(APEX_PROBE_GCC AND APEX_PROBE_GXX AND APEX_PROBE_CLANG AND APEX_PROBE_CLANGXX)
    set(_probe_dir ${CMAKE_CURRENT_BINARY_DIR}/plugin-abi-probe)
    file(MAKE_DIRECTORY ${_probe_dir})
    add_custom_command(OUTPUT ${_probe_dir}/gcc-plugin.so ${_probe_dir}/clang-plugin.so ${_probe_dir}/gcc-host ${_probe_dir}/clang-host
      COMMAND ${APEX_PROBE_GCC} -shared -fPIC ${CMAKE_CURRENT_SOURCE_DIR}/plugin_host/probe/stub.c -o ${_probe_dir}/gcc-plugin.so
      COMMAND ${APEX_PROBE_CLANG} -shared -fPIC ${CMAKE_CURRENT_SOURCE_DIR}/plugin_host/probe/stub.c -o ${_probe_dir}/clang-plugin.so
      COMMAND ${APEX_PROBE_GXX} ${CMAKE_CURRENT_SOURCE_DIR}/plugin_host/probe/host.cpp -ldl -o ${_probe_dir}/gcc-host
      COMMAND ${APEX_PROBE_CLANGXX} ${CMAKE_CURRENT_SOURCE_DIR}/plugin_host/probe/host.cpp -ldl -o ${_probe_dir}/clang-host
      DEPENDS plugin_host/probe/stub.c plugin_host/probe/host.cpp VERBATIM)
    add_custom_target(apex_plugin_abi_probe DEPENDS ${_probe_dir}/gcc-plugin.so ${_probe_dir}/clang-plugin.so ${_probe_dir}/gcc-host ${_probe_dir}/clang-host)
    add_test(NAME plugin_abi_gcc_host_clang_plugin COMMAND ${_probe_dir}/gcc-host ${_probe_dir}/clang-plugin.so)
    add_test(NAME plugin_abi_clang_host_gcc_plugin COMMAND ${_probe_dir}/clang-host ${_probe_dir}/gcc-plugin.so)
  endif()
endif()
