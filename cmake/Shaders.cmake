# apex_compile_shaders(TARGET <layer target> DIR <dir> HEADER <path under gen/>)
#
# Compiles every <dir>/<name>.<stage>.glsl with glslc into SPIR-V (plan 0.1, D14) and generates a
# header at ${CMAKE_BINARY_DIR}/gen/<HEADER> with one `inline constexpr std::uint32_t <name>_<stage>[]`
# array per shader, named after the file with '.' replaced by '_'. The layer target is made to build
# after the header exists. Debug builds also define APEX_SHADER_DIR, the source directory, so the
# renderer can hot-reload shaders from disk.
#
# Stage comes from the second-to-last extension: flat.vert.glsl is a vertex shader.
function(apex_compile_shaders)
  cmake_parse_arguments(S "" "TARGET;DIR;HEADER" "INCLUDES" ${ARGN})
  if(NOT S_TARGET OR NOT S_DIR OR NOT S_HEADER)
    message(FATAL_ERROR "apex_compile_shaders needs TARGET, DIR and HEADER")
  endif()

  find_program(APEX_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin" DOC "Vulkan SDK shader compiler")
  if(NOT APEX_GLSLC)
    message(FATAL_ERROR "glslc not found; install the Vulkan SDK or set VULKAN_SDK")
  endif()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  file(GLOB shader_sources CONFIGURE_DEPENDS "${S_DIR}/*.glsl")
  file(GLOB shader_includes CONFIGURE_DEPENDS "${S_DIR}/*.inc")
  list(APPEND shader_includes ${S_INCLUDES})
  set(gen_dir "${CMAKE_BINARY_DIR}/gen")
  set(spv_dir "${gen_dir}/spv")
  set(header "${gen_dir}/${S_HEADER}")

  set(spv_files "")
  set(embed_args "")
  foreach(source IN LISTS shader_sources)
    get_filename_component(file_name "${source}" NAME)
    if(NOT file_name MATCHES "^(.+)\\.(vert|frag|comp|geom|tesc|tese|task|mesh)\\.glsl$")
      message(FATAL_ERROR "shader '${file_name}' must be named <name>.<vert|frag|comp|...>.glsl")
    endif()
    set(id "${CMAKE_MATCH_1}_${CMAKE_MATCH_2}")
    set(stage "${CMAKE_MATCH_2}")
    set(spv "${spv_dir}/${id}.spv")
    add_custom_command(
      OUTPUT "${spv}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${spv_dir}"
      COMMAND "${APEX_GLSLC}" -fshader-stage=${stage} --target-env=vulkan1.2 -I "${S_DIR}" -o "${spv}" "${source}"
      DEPENDS "${source}" ${shader_includes}
      COMMENT "Compiling shader ${file_name}"
      VERBATIM)
    list(APPEND spv_files "${spv}")
    list(APPEND embed_args "${id}=${spv}")
  endforeach()

  add_custom_command(
    OUTPUT "${header}"
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/embed_spirv.py" "${header}" ${embed_args}
    DEPENDS ${spv_files} "${PROJECT_SOURCE_DIR}/tools/embed_spirv.py"
    COMMENT "Embedding SPIR-V into ${S_HEADER}"
    VERBATIM)
  add_custom_target(${S_TARGET}_shaders DEPENDS "${header}")
  add_dependencies(${S_TARGET} ${S_TARGET}_shaders)

  target_compile_definitions(${S_TARGET} PRIVATE
    "$<$<CONFIG:Debug>:APEX_SHADER_DIR=\"${S_DIR}\">")
endfunction()
