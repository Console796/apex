cmake_minimum_required(VERSION 3.25)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/version.txt APEX_VERSION_TXT LIMIT_COUNT 1)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${APEX_VERSION_TXT}")
set(APEX_VERSION_MAJOR ${CMAKE_MATCH_1})
set(APEX_VERSION_MILESTONE ${CMAKE_MATCH_2})
set(APEX_VERSION_SUB ${CMAKE_MATCH_3})

project(Apex VERSION ${APEX_VERSION_MAJOR}.${APEX_VERSION_MILESTONE}.${APEX_VERSION_SUB}
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(APEX_BUILD_TESTS "Build tests" ON)
option(APEX_BUILD_APP "Build the Qt application (needs Qt 6)" ON)
set(APEX_SANITIZE "" CACHE STRING "Sanitizers, e.g. address;undefined")
# BUILD field of the version (plan 0.1, D1): injected by tools/ci.py, 0 for ad-hoc builds.
set(APEX_BUILD_NUMBER 0 CACHE STRING "CI run counter")

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Layers)

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
           DEPS glm::glm spdlog::spdlog)
target_include_directories(apex_core PUBLIC ${CMAKE_BINARY_DIR}/gen)
apex_layer(geometry DEPS apex_core)
apex_layer(document DEPS apex_geometry apex_core)
apex_layer(assets DEPS apex_core)
apex_layer(render DEPS apex_document apex_assets)
apex_layer(viewport DEPS apex_render apex_document)

if(APEX_BUILD_APP)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
  apex_layer(ui SOURCES src/ui/main_window.cpp DEPS apex_viewport Qt6::Widgets)
  set_target_properties(apex_ui PROPERTIES AUTOMOC ON)
  add_executable(apex src/app/main.cpp)
  target_link_libraries(apex PRIVATE apex_ui apex_warnings)
endif()

if(APEX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
