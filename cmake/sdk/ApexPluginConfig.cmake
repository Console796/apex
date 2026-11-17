# ApexPluginConfig.cmake: the Apex plugin SDK as a CMake package (plugins.md D3, 0.9 D8).
#
#   find_package(ApexPlugin REQUIRED)            # with CMAKE_PREFIX_PATH pointing at the unpacked SDK
#   apex_add_plugin(my_tool ID mytool NAME "My tool" SOURCES my_tool.cpp)
#
# Provides the imported INTERFACE target apex::plugin_sdk (the include directory holding apex/apex_plugin.h and
# apex/plugin.hpp) and the apex_add_plugin() and apex_add_script_plugin() functions
# (ApexPluginHelpers.cmake, installed beside this file).
get_filename_component(_apex_plugin_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT EXISTS "${_apex_plugin_prefix}/include/apex/apex_plugin.h")
  set(ApexPlugin_FOUND FALSE)
  set(ApexPlugin_NOT_FOUND_MESSAGE "The Apex plugin SDK headers were not found under ${_apex_plugin_prefix}/include/apex")
  return()
endif()
if(NOT TARGET apex::plugin_sdk)
  add_library(apex::plugin_sdk INTERFACE IMPORTED)
  set_target_properties(apex::plugin_sdk PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_apex_plugin_prefix}/include")
endif()
set(ApexPlugin_INCLUDE_DIR "${_apex_plugin_prefix}/include")
set(ApexPlugin_EXAMPLES_DIR "${_apex_plugin_prefix}/share/apex-sdk/examples")
include("${CMAKE_CURRENT_LIST_DIR}/ApexPluginHelpers.cmake")
unset(_apex_plugin_prefix)
