cmake_minimum_required(VERSION 3.25)


file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/version.txt APEX_VERSION_TXT LIMIT_COUNT 1)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${APEX_VERSION_TXT}")
set(APEX_VERSION_MAJOR ${CMAKE_MATCH_1})
set(APEX_VERSION_MILESTONE ${CMAKE_MATCH_2})
set(APEX_VERSION_SUB ${CMAKE_MATCH_3})

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
                       src/core/log.cpp src/core/job_pool.cpp src/core/json.cpp
                       src/core/kv1.cpp src/core/kv1_parse.cpp src/core/kv1_write.cpp
           DEPS glm::glm spdlog::spdlog)
target_include_directories(apex_core PUBLIC ${CMAKE_BINARY_DIR}/gen)
apex_layer(geometry SOURCES src/geometry/convex_polyhedron.cpp src/geometry/face_matching.cpp
                           src/geometry/solid_geometry.cpp src/geometry/bvh.cpp
           DEPS apex_core)
apex_layer(document SOURCES src/document/vmf.cpp src/document/document.cpp DEPS apex_geometry apex_core)
apex_layer(editor SOURCES src/editor/editor_session.cpp src/editor/visibility.cpp src/editor/pick_index.cpp
                          src/editor/group_index.cpp src/editor/selection.cpp src/editor/selection_expand.cpp
                          src/editor/selection_ops.cpp src/editor/selection_undo.cpp src/editor/actions.cpp
                          src/editor/work_plane.cpp src/editor/group_ops.cpp src/editor/hide_ops.cpp src/editor/visgroups.cpp src/editor/clipboard.cpp
                          src/editor/transform.cpp src/editor/preview.cpp
                          src/editor/gizmo.cpp src/editor/modal_transform.cpp
                          src/editor/inspector_model.cpp src/editor/placement.cpp
           DEPS apex_document apex_geometry apex_core)
apex_layer(assets SOURCES src/assets/game_profile.cpp src/assets/vfs.cpp
                         src/assets/vmt.cpp src/assets/vtf.cpp src/assets/fgd.cpp
                         src/assets/material_cache.cpp DEPS apex_core)
target_link_libraries(apex_assets PRIVATE sourcepp::vpkpp sourcepp::steampp
                                          sourcepp::vtfpp sourcepp::toolpp)
apex_layer(render SOURCES src/render/shader_blobs.cpp src/render/vma_impl.cpp src/render/vk.cpp
                         src/render/context.cpp src/render/resources.cpp
                         src/render/upload_ring.cpp src/render/texture_heap.cpp
                         src/render/render_target.cpp src/render/swapchain.cpp
                         src/render/renderer.cpp src/render/solid_mesh.cpp src/render/render_mirror.cpp
           DEPS apex_document apex_assets volk::volk GPUOpen::VulkanMemoryAllocator
                Vulkan::Headers)
# GLSL in src/render/shaders is compiled to SPIR-V and embedded in gen/render/shaders_spv.hpp (D14).
apex_compile_shaders(TARGET apex_render DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders
                     HEADER render/shaders_spv.hpp)
apex_layer(viewport SOURCES src/viewport/camera.cpp src/viewport/viewport.cpp
                         src/viewport/controllers.cpp src/viewport/view_context.cpp
                         src/viewport/layout.cpp src/viewport/map_views.cpp src/viewport/pick_math.cpp
           DEPS apex_render apex_document)

if(APEX_BUILD_APP)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
  apex_layer(ui SOURCES src/ui/main_window.cpp src/ui/theme.cpp src/ui/viewport_area.cpp src/ui/viewport_pane.cpp src/ui/gpu_host.cpp
                        src/ui/key_chord.cpp src/ui/transform_input.cpp src/ui/transform_dialog.cpp src/ui/paste_special_dialog.cpp src/ui/tie_dialog.cpp src/ui/outliner_panel.cpp src/ui/visgroup_panel.cpp src/ui/history_panel.cpp src/ui/log_panel.cpp
             DEPS apex_viewport apex_editor Qt6::Widgets ads::qtadvanceddocking-qt6)
  set_target_properties(apex_ui PROPERTIES AUTOMOC ON)
  add_executable(apex src/app/main.cpp)
  target_link_libraries(apex PRIVATE apex_ui apex_warnings)
  if(APEX_BUILD_TESTS)
    # Headless shell tests (QT_QPA_PLATFORM=offscreen).
    add_executable(apex_ui_tests tests/main.cpp tests/ui/shell_test.cpp)
    target_link_libraries(apex_ui_tests PRIVATE apex_ui apex_warnings doctest::doctest)
    target_compile_definitions(apex_ui_tests PRIVATE
                               APEX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME apex_ui_tests COMMAND apex_ui_tests)
    set_tests_properties(apex_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0")
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
  else()
    message(STATUS "APEX_BUILD_FUZZ requires Clang; skipping fuzz targets")
  endif()
endif()
