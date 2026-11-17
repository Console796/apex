# apex_add_plugin() and apex_add_script_plugin(): build a plugin folder as plugins.md D9 lays it out.
#
#   <OUTPUT_DIRECTORY>/<FOLDER>/plugin.kv3                 the manifest, generated
#   <OUTPUT_DIRECTORY>/<FOLDER>/bin/<platform>/<name>.so   the library (.dll on Windows)
#
# <platform> is linux-x86_64 or windows-x86_64 and the manifest's entry names it, so a folder built on each
# platform can be merged into one that loads on both. The ABI is 64-bit only.
#
# Included by ApexPluginConfig.cmake (installed SDK) and by the Apex build itself for its examples, so an
# example built in the Apex tree is built exactly as a third party builds it.
cmake_policy(PUSH)
cmake_policy(VERSION 3.20)

if(WIN32)
  set(_APEX_PLUGIN_PLATFORM "windows-x86_64")
else()
  set(_APEX_PLUGIN_PLATFORM "linux-x86_64")
endif()

# apex_add_plugin(<target>
#     ID <id>                      manifest id: dot-separated letters, digits, '_' and '-' (required)
#     NAME <display name>          (required)
#     SOURCES <files...>           C or C++ sources (required)
#     [VERSION <x.y.z>]            plugin version (default 1.0.0)
#     [FOLDER <name>]              folder name under OUTPUT_DIRECTORY (default: the id)
#     [OUTPUT_DIRECTORY <dir>]     default: ${APEX_PLUGIN_OUTPUT_DIRECTORY}, else <build dir>/plugins
#     [ABI_MAJOR <n> ABI_MINOR <n>])  what the plugin was written against (default 1 and 0)
function(apex_add_plugin target)
  cmake_parse_arguments(P "" "ID;NAME;VERSION;FOLDER;OUTPUT_DIRECTORY;ABI_MAJOR;ABI_MINOR" "SOURCES" ${ARGN})
  if(NOT P_ID OR NOT P_NAME OR NOT P_SOURCES)
    message(FATAL_ERROR "apex_add_plugin(${target}): ID, NAME and SOURCES are required")
  endif()
  if(NOT P_ID MATCHES "^[A-Za-z0-9_-]+(\\.[A-Za-z0-9_-]+)*$")
    message(FATAL_ERROR "apex_add_plugin(${target}): '${P_ID}' is not a valid plugin id")
  endif()
  if(NOT P_VERSION)
    set(P_VERSION "1.0.0")
  endif()
  if(NOT P_FOLDER)
    set(P_FOLDER "${P_ID}")
  endif()
  if(NOT P_OUTPUT_DIRECTORY)
    if(APEX_PLUGIN_OUTPUT_DIRECTORY)
      set(P_OUTPUT_DIRECTORY "${APEX_PLUGIN_OUTPUT_DIRECTORY}")
    else()
      set(P_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins")
    endif()
  endif()
  if(NOT DEFINED P_ABI_MAJOR)
    set(P_ABI_MAJOR 1)
  endif()
  if(NOT DEFINED P_ABI_MINOR)
    set(P_ABI_MINOR 0)
  endif()
  set(folder "${P_OUTPUT_DIRECTORY}/${P_FOLDER}")
  set(libdir "${folder}/bin/${_APEX_PLUGIN_PLATFORM}")

  add_library(${target} MODULE ${P_SOURCES})
  target_link_libraries(${target} PRIVATE apex::plugin_sdk)
  # Only the entry point is exported (APEX_PLUGIN_EXPORT in apex_plugin.h); everything else is hidden so two
  # plugins, or a plugin and the host, never interpose each other's symbols.
  set_target_properties(${target} PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    # The genex also stops multi-config generators from adding a per-configuration subdirectory.
    LIBRARY_OUTPUT_DIRECTORY "$<1:${libdir}>" RUNTIME_OUTPUT_DIRECTORY "$<1:${libdir}>")
  # plugin.hpp needs C++23; a C-only plugin must not ask for a C++ compile feature.
  foreach(source IN LISTS P_SOURCES)
    if(source MATCHES "\\.(cpp|cc|cxx|C)$")
      target_compile_features(${target} PRIVATE cxx_std_23)
      break()
    endif()
  endforeach()
  file(GENERATE OUTPUT "${folder}/plugin.kv3" CONTENT
"<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->
{ id = \"${P_ID}\" name = \"${P_NAME}\" version = \"${P_VERSION}\" apex_abi = { major = ${P_ABI_MAJOR} minor = ${P_ABI_MINOR} } entry = { \"${_APEX_PLUGIN_PLATFORM}\" = \"bin/${_APEX_PLUGIN_PLATFORM}/$<TARGET_FILE_NAME:${target}>\" } }
")
endfunction()

# apex_add_script_plugin(<target>
#     ID <id> NAME <name> SCRIPT <file> [RUNTIME angelscript] [VERSION <x.y.z>] [FOLDER <name>] [OUTPUT_DIRECTORY <dir>])
# A script plugin has no library: the manifest names the script, which is copied next to it.
function(apex_add_script_plugin target)
  cmake_parse_arguments(P "" "ID;NAME;SCRIPT;RUNTIME;VERSION;FOLDER;OUTPUT_DIRECTORY;ABI_MAJOR;ABI_MINOR" "" ${ARGN})
  if(NOT P_ID OR NOT P_NAME OR NOT P_SCRIPT)
    message(FATAL_ERROR "apex_add_script_plugin(${target}): ID, NAME and SCRIPT are required")
  endif()
  if(NOT P_RUNTIME)
    set(P_RUNTIME "angelscript")
  endif()
  if(NOT P_VERSION)
    set(P_VERSION "1.0.0")
  endif()
  if(NOT P_FOLDER)
    set(P_FOLDER "${P_ID}")
  endif()
  if(NOT P_OUTPUT_DIRECTORY)
    if(APEX_PLUGIN_OUTPUT_DIRECTORY)
      set(P_OUTPUT_DIRECTORY "${APEX_PLUGIN_OUTPUT_DIRECTORY}")
    else()
      set(P_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins")
    endif()
  endif()
  if(NOT DEFINED P_ABI_MAJOR)
    set(P_ABI_MAJOR 1)
  endif()
  if(NOT DEFINED P_ABI_MINOR)
    set(P_ABI_MINOR 0)
  endif()
  get_filename_component(script_name "${P_SCRIPT}" NAME)
  get_filename_component(script_abs "${P_SCRIPT}" ABSOLUTE)
  set(folder "${P_OUTPUT_DIRECTORY}/${P_FOLDER}")
  file(GENERATE OUTPUT "${folder}/plugin.kv3" CONTENT
"<!-- kv3 encoding:text:version{apex} format:generic:version{fixture} -->
{ id = \"${P_ID}\" name = \"${P_NAME}\" version = \"${P_VERSION}\" apex_abi = { major = ${P_ABI_MAJOR} minor = ${P_ABI_MINOR} } script = \"${script_name}\" runtime = \"${P_RUNTIME}\" }
")
  add_custom_target(${target} ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${folder}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${script_abs}" "${folder}/${script_name}"
    DEPENDS "${script_abs}"
    COMMENT "Installing the ${P_ID} script plugin")
endfunction()

cmake_policy(POP)
