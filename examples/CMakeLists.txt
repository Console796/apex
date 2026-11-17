# The example plugins (docs/plugins/examples.md). Two ways to build this file:
#
#   * In the Apex tree, add_subdirectory(examples) builds them as ordinary targets (the loader and API tests use
#     them) through apex_add_plugin(), the same helper the SDK ships.
#   * From the SDK, as its own project against the installed package:
#       cmake -S <sdk>/share/apex-sdk/examples -B build -DCMAKE_PREFIX_PATH=<sdk>
#     The out-of-tree test (tests/plugin_sdk/out_of_tree.py) does exactly this.
#
# APEX_EXAMPLE_PLUGINS (a list of target names) limits what is built; the default is every example.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(apex_plugin_examples LANGUAGES C CXX)
  find_package(ApexPlugin REQUIRED)
endif()

set(_examples hello_tool polygon_tool face_stats obj_import)
if(NOT APEX_EXAMPLE_PLUGINS)
  set(APEX_EXAMPLE_PLUGINS ${_examples} stair_plugin)
endif()

if("hello_tool" IN_LIST APEX_EXAMPLE_PLUGINS)
  apex_add_plugin(hello_tool ID hello NAME "Hello tool" FOLDER hello SOURCES hello-tool/hello.c)
endif()
if("polygon_tool" IN_LIST APEX_EXAMPLE_PLUGINS)
  apex_add_plugin(polygon_tool ID polygon NAME "Polygon tool" FOLDER polygon SOURCES polygon-tool/polygon.cpp)
endif()
if("face_stats" IN_LIST APEX_EXAMPLE_PLUGINS)
  apex_add_plugin(face_stats ID facestats NAME "Face stats" FOLDER face-stats SOURCES face-stats/face_stats.cpp)
endif()
if("obj_import" IN_LIST APEX_EXAMPLE_PLUGINS)
  apex_add_plugin(obj_import ID objimport NAME "OBJ import" FOLDER obj-import SOURCES obj-import/obj_import.cpp)
endif()
# A script plugin needs no compiler. The Apex tree builds it only when its AngelScript engine is on.
if("stair_plugin" IN_LIST APEX_EXAMPLE_PLUGINS AND (NOT DEFINED APEX_WITH_ANGELSCRIPT OR APEX_WITH_ANGELSCRIPT))
  apex_add_script_plugin(stair_plugin ID stair NAME "Stair" FOLDER stair SCRIPT stair/stair.as RUNTIME angelscript)
endif()
