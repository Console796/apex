# apex_layer(<name> [SOURCES ...] [DEPS ...]) defines static library apex_<name> for a layer.
# Include root is src/, so code includes "core/handle.hpp". With no sources the target is an
# INTERFACE library, so empty layers still exist in the link graph.
function(apex_layer name)
  cmake_parse_arguments(L "" "" "SOURCES;DEPS" ${ARGN})
  set(target apex_${name})
  if(L_SOURCES)
    add_library(${target} STATIC ${L_SOURCES})
    target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${target} PUBLIC ${L_DEPS})
    target_link_libraries(${target} PRIVATE apex_warnings)
  else()
    add_library(${target} INTERFACE)
    target_include_directories(${target} INTERFACE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${target} INTERFACE ${L_DEPS})
  endif()
  add_library(apex::${name} ALIAS ${target})
endfunction()
