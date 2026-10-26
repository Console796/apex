cmake_minimum_required(VERSION 3.25)


file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/version.txt APEX_VERSION_TXT LIMIT_COUNT 1)
# MAJOR.MILESTONE.SUB, or MAJOR.MILESTONE.SUB-release for a milestone's release (roadmap, Versioning).
if(NOT APEX_VERSION_TXT MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-release)?$")
  message(FATAL_ERROR "version.txt: expected MAJOR.MILESTONE.SUB[-release], got '${APEX_VERSION_TXT}'")
endif()
set(APEX_VERSION_MAJOR ${CMAKE_MATCH_1})
set(APEX_VERSION_MILESTONE ${CMAKE_MATCH_2})
set(APEX_VERSION_SUB ${CMAKE_MATCH_3})
set(APEX_VERSION_SUFFIX "${CMAKE_MATCH_4}")
if(APEX_VERSION_SUFFIX)
  set(APEX_VERSION_RELEASE 1)
else()
  set(APEX_VERSION_RELEASE 0)
endif()

project(Apex VERSION ${APEX_VERSION_MAJOR}.${APEX_VERSION_MILESTONE}.${APEX_VERSION_SUB}
        LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(APEX_BUILD_TESTS "Build tests" ON)
option(APEX_BUILD_FUZZ "Build the Clang libFuzzer codec targets" OFF)
option(APEX_BUILD_APP "Build the Qt application (needs Qt 6)" ON)
set(APEX_SANITIZE "" CACHE STRING "Sanitizers, e.g. address;undefined")
# BUILD field of the version (plan 0.1, D1): injected by tools/ci.py, 0 for ad-hoc builds.
set(APEX_BUILD_NUMBER 0 CACHE STRING "CI run counter")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Layers)
include(Shaders)

# Git hash and dirty flag for apex/version.hpp.
find_package(Git QUIET)
set(APEX_GIT_HASH "unknown")
set(APEX_GIT_DIRTY 0)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE _hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE _status OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_hash)
    set(APEX_GIT_HASH ${_hash})
  endif()
  if(_status)
    set(APEX_GIT_DIRTY 1)
  endif()
endif()
configure_file(cmake/version.hpp.in ${CMAKE_BINARY_DIR}/gen/apex/version.hpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/version.txt ${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD)

# Warnings and sanitizers apply to our targets only, through this interface library.
add_library(apex_warnings INTERFACE)
if(MSVC)
  target_compile_options(apex_warnings INTERFACE /W4 /permissive- /utf-8)
else()
  target_compile_options(apex_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow
                         -Wconversion -Wno-sign-conversion)
endif()
if(APEX_SANITIZE)
  string(REPLACE ";" "," _san "${APEX_SANITIZE}")
  if(MSVC)
    target_compile_options(apex_warnings INTERFACE /fsanitize=address)
  else()
    target_compile_options(apex_warnings INTERFACE -fsanitize=${_san} -fno-omit-frame-pointer)
    target_link_options(apex_warnings INTERFACE -fsanitize=${_san})
  endif()
endif()

include(deps)

# Layer graph, plan 0.1 section 2. tools/check_layers.py enforces the same edges on #includes.
apex_layer(core SOURCES src/core/version.cpp src/core/strings.cpp src/core/paths.cpp
                       src/core/log.cpp src/core/job_pool.cpp src/core/json.cpp src/core/kv3.cpp
                       src/core/kv1.cpp src/core/kv1_parse.cpp src/core/kv1_write.cpp
           DEPS glm::glm spdlog::spdlog)
target_include_directories(apex_core PUBLIC ${CMAKE_BINARY_DIR}/gen)
apex_layer(geometry SOURCES src/geometry/convex_polyhedron.cpp src/geometry/face_uv.cpp src/geometry/face_matching.cpp
                           src/geometry/solid_geometry.cpp src/geometry/bvh.cpp
                           src/geometry/convex_hull.cpp src/geometry/solid_block.cpp src/geometry/primitives.cpp
           DEPS apex_core)
apex_layer(document SOURCES src/document/vmf.cpp src/document/document.cpp src/document/entity_transform.cpp DEPS apex_geometry apex_core)
apex_layer(editor SOURCES src/editor/autosave.cpp src/editor/editor_session.cpp src/editor/tool.cpp src/editor/select_tool.cpp src/editor/entity_tool.cpp src/editor/draw_extrude_tool.cpp src/editor/primitive_tool.cpp src/editor/clip_tool.cpp src/editor/face_edit.cpp src/editor/apply_tool.cpp src/editor/measure_tool.cpp src/editor/topology_ops.cpp src/editor/vertex_edit.cpp src/editor/vertex_tool.cpp src/editor/visibility.cpp src/editor/pick_index.cpp src/editor/pick_model.cpp
                          src/editor/pie_menu.cpp src/editor/pie_menus.cpp
                          src/editor/group_index.cpp src/editor/selection.cpp src/editor/selection_expand.cpp
                          src/editor/selection_ops.cpp src/editor/selection_undo.cpp src/editor/actions.cpp
                          src/editor/snapper.cpp src/editor/work_plane.cpp src/editor/group_ops.cpp src/editor/hide_ops.cpp src/editor/visgroups.cpp src/editor/clipboard.cpp
                          src/editor/transform.cpp src/editor/preview.cpp src/editor/preset_io.cpp
                          src/editor/gizmo.cpp src/editor/modal_transform.cpp
                          src/editor/inspector_model.cpp src/editor/placement.cpp
           DEPS apex_document apex_geometry apex_core)
apex_layer(assets SOURCES src/assets/game_profile.cpp src/assets/vfs.cpp src/assets/addons.cpp
                         src/assets/vmt.cpp src/assets/vtf.cpp src/assets/fgd.cpp src/assets/model_bake.cpp src/assets/model_pose.cpp
                         src/assets/material_cache.cpp src/assets/model_cache.cpp src/assets/material_index.cpp src/assets/thumbnail_cache.cpp DEPS apex_core)
target_link_libraries(apex_assets PRIVATE sourcepp::vpkpp sourcepp::steampp
                                          sourcepp::vtfpp sourcepp::toolpp sourcepp::mdlpp sourcepp::kvpp sourcepp::bsppp)
apex_layer(render SOURCES src/render/shader_blobs.cpp src/render/vma_impl.cpp src/render/vk.cpp
                         src/render/context.cpp src/render/resources.cpp
                         src/render/upload_ring.cpp src/render/texture_heap.cpp
                         src/render/render_target.cpp src/render/swapchain.cpp
                         src/render/renderer.cpp src/render/glyph_atlas.cpp src/render/solid_mesh.cpp src/render/render_mirror.cpp
           DEPS apex_document apex_assets volk::volk GPUOpen::VulkanMemoryAllocator
                Vulkan::Headers)
# GLSL in src/render/shaders is compiled to SPIR-V and embedded in gen/render/shaders_spv.hpp (D14).
apex_compile_shaders(TARGET apex_render DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders
                     HEADER render/shaders_spv.hpp)
apex_layer(viewport SOURCES src/viewport/camera.cpp src/viewport/viewport.cpp
                         src/viewport/controllers.cpp src/viewport/view_context.cpp
                         src/viewport/layout.cpp src/viewport/map_views.cpp src/viewport/workspace.cpp src/viewport/pick_math.cpp
           DEPS apex_render apex_document)

if(APEX_BUILD_APP)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Svg)
  apex_layer(ui SOURCES src/ui/recovery_dialog.cpp src/ui/main_window.cpp src/ui/game_content.cpp src/ui/materials_panel.cpp src/ui/face_properties_panel.cpp src/ui/theme.cpp src/ui/theme_editor.cpp src/ui/viewport_area.cpp src/ui/viewport_pane.cpp src/ui/tool_parameter_strip.cpp src/ui/gpu_host.cpp src/ui/model_pick.cpp
                        src/ui/uv_view.cpp src/ui/pointer_lock.cpp src/ui/input_log.cpp
                        src/ui/pie_menu_popup.cpp src/ui/key_chord.cpp src/ui/settings_dialog.cpp src/ui/transform_input.cpp src/ui/transform_dialog.cpp src/ui/paste_special_dialog.cpp src/ui/tie_dialog.cpp src/ui/outliner_panel.cpp src/ui/visgroup_panel.cpp src/ui/history_panel.cpp src/ui/inspector_panel.cpp src/ui/log_panel.cpp src/ui/area_frame.cpp src/ui/workspace_bar.cpp
             DEPS apex_viewport apex_editor Qt6::Widgets Qt6::Svg)
  set_target_properties(apex_ui PROPERTIES AUTOMOC ON AUTORCC ON)
  target_sources(apex_ui PRIVATE src/ui/icons/icons.qrc)
  # Pointer lock is optional: other platforms and minimal Linux builds retain local input.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
      pkg_check_modules(WAYLAND_SCANNER QUIET wayland-scanner)
      if(WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER_FOUND)
        find_package(Qt6 6.5 QUIET COMPONENTS GuiPrivate)
      endif()
      if(WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER_FOUND AND TARGET Qt6::GuiPrivate)
        pkg_get_variable(_wayland_scanner wayland-scanner wayland_scanner)
        if(EXISTS "${_wayland_scanner}")
          enable_language(C)
          set(_protocol_gen ${CMAKE_CURRENT_BINARY_DIR}/gen/wayland)
          file(MAKE_DIRECTORY ${_protocol_gen})
          foreach(_protocol pointer-constraints-unstable-v1 relative-pointer-unstable-v1)
            set(_xml ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wayland-protocols/${_protocol}.xml)
            set(_header ${_protocol_gen}/${_protocol}-client-protocol.h)
            set(_code ${_protocol_gen}/${_protocol}-protocol.c)
            add_custom_command(OUTPUT ${_header} ${_code}
              COMMAND ${_wayland_scanner} client-header ${_xml} ${_header}
              COMMAND ${_wayland_scanner} private-code ${_xml} ${_code}
              DEPENDS ${_xml} VERBATIM)
            target_sources(apex_ui PRIVATE ${_header} ${_code})
          endforeach()
          target_sources(apex_ui PRIVATE src/ui/pointer_lock_wayland.cpp)
          target_include_directories(apex_ui PRIVATE ${_protocol_gen})
          target_link_libraries(apex_ui PRIVATE PkgConfig::WAYLAND_CLIENT Qt6::GuiPrivate)
          target_compile_definitions(apex_ui PRIVATE APEX_HAS_WAYLAND_POINTER_LOCK)
        endif()
      endif()
    endif()
  endif()
  add_executable(apex src/app/main.cpp)
  target_link_libraries(apex PRIVATE apex_ui apex_warnings)
  if(APEX_BUILD_TESTS)
    # Headless shell tests (QT_QPA_PLATFORM=offscreen).
    add_executable(apex_ui_tests tests/main.cpp tests/ui/shell_test.cpp tests/ui/materials_test.cpp tests/ui/face_properties_test.cpp tests/ui/recovery_test.cpp tests/ui/pointer_lock_test.cpp tests/ui/settings_test.cpp tests/ui/primitive_test.cpp tests/ui/topology_test.cpp tests/ui/tool_controls_test.cpp tests/ui/pie_popup_test.cpp)
    target_link_libraries(apex_ui_tests PRIVATE apex_ui sourcepp::vtfpp apex_warnings doctest::doctest)
    target_compile_definitions(apex_ui_tests PRIVATE
                               APEX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME apex_ui_tests COMMAND apex_ui_tests)
    set_tests_properties(apex_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0;APEX_SKIP_RECOVERY=1")
    if(WIN32)
      # Shared Qt DLLs are not on the guest's PATH.
      set_tests_properties(apex_ui_tests PROPERTIES ENVIRONMENT_MODIFICATION
                           "PATH=path_list_prepend:${Qt6_DIR}/../../../bin")
    endif()
  endif()
endif()

include(Packaging)

if(APEX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(APEX_BUILD_TESTS)
  add_executable(apex_replane tools/replane/main.cpp)
  target_link_libraries(apex_replane PRIVATE apex_document apex_geometry apex_core apex_warnings)
  add_executable(apex_vmf_dump tools/vmf_dump/main.cpp)
  target_link_libraries(apex_vmf_dump PRIVATE apex_document apex_geometry apex_core apex_warnings)
endif()

if(APEX_BUILD_FUZZ)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Compile the codec itself with coverage and sanitizers, independently of apex_core.
    add_executable(kv1_fuzz tests/fuzz/kv1_fuzz.cpp src/core/kv1.cpp src/core/kv1_parse.cpp
                   src/core/kv1_write.cpp src/core/strings.cpp src/core/paths.cpp)
    target_include_directories(kv1_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(kv1_fuzz PRIVATE apex_warnings)
    target_compile_options(kv1_fuzz PRIVATE -fsanitize=fuzzer,address,undefined
                           -fno-omit-frame-pointer)
    target_link_options(kv1_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    add_executable(vmf_fuzz tests/fuzz/vmf_fuzz.cpp src/document/vmf.cpp src/core/kv1.cpp
                   src/core/kv1_parse.cpp src/core/kv1_write.cpp src/core/strings.cpp
                   src/core/paths.cpp)
    target_include_directories(vmf_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(vmf_fuzz PRIVATE apex_warnings)
    target_compile_options(vmf_fuzz PRIVATE -fsanitize=fuzzer,address,undefined
                           -fno-omit-frame-pointer)
    target_link_options(vmf_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    add_executable(hull_fuzz tests/fuzz/hull_fuzz.cpp src/geometry/convex_hull.cpp
                   src/geometry/convex_polyhedron.cpp)
    target_include_directories(hull_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(hull_fuzz PRIVATE glm::glm apex_warnings)
    target_compile_options(hull_fuzz PRIVATE -fsanitize=fuzzer,address,undefined
                           -fno-omit-frame-pointer)
    target_link_options(hull_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  else()
    message(STATUS "APEX_BUILD_FUZZ requires Clang; skipping fuzz targets")
  endif()
endif()
