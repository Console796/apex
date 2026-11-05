# All source dependencies, pinned by exact commit (plan 0.1, D3). Qt and Vulkan come from the
# system via find_package. Every name added here must also appear in THIRD_PARTY.md.
include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

set(CPM_USE_NAMED_CACHE_DIRECTORIES ON)

if(APEX_BUILD_TESTS)
  # doctest v2.4.12
  CPMAddPackage(
    NAME doctest
    GITHUB_REPOSITORY doctest/doctest
    GIT_TAG 1da23a3e8119ec5cce4f9388e91b065e20bf06f5
    OPTIONS "DOCTEST_WITH_TESTS OFF" "DOCTEST_NO_INSTALL ON"
  )
  list(APPEND CMAKE_MODULE_PATH ${doctest_SOURCE_DIR}/scripts/cmake)
endif()

# glm 1.0.1
CPMAddPackage(
  NAME glm
  GITHUB_REPOSITORY g-truc/glm
  GIT_TAG 0af55ccecd98d4e5a8d1fad7de25ba429d60e863
)

# spdlog v1.15.3
CPMAddPackage(
  NAME spdlog
  GITHUB_REPOSITORY gabime/spdlog
  GIT_TAG 6fa36017cfd5731d617e1a934f0e5ea9c4445b13
  OPTIONS "SPDLOG_INSTALL OFF" "SPDLOG_BUILD_EXAMPLE OFF" "SPDLOG_BUILD_TESTS OFF"
)

# SourcePP v2026.9.11: asset readers (vpkpp, steampp, vtfpp, toolpp, mdlpp, kvpp for DMX, bsppp for BSP entity lumps).
set(SOURCEPP_LIBS_START_ENABLED OFF CACHE BOOL "" FORCE)
set(SOURCEPP_USE_VPKPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_STEAMPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_VTFPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_TOOLPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_MDLPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_KVPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_BSPPP ON CACHE BOOL "" FORCE)
# We only decode VTFs; the image-import codecs are unused (and libjxl rejects mixed compilers).
set(SOURCEPP_VTFPP_SUPPORT_EXR OFF CACHE BOOL "" FORCE)
set(SOURCEPP_VTFPP_SUPPORT_JXL OFF CACHE BOOL "" FORCE)
set(SOURCEPP_VTFPP_SUPPORT_WEBP OFF CACHE BOOL "" FORCE)
set(SOURCEPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
CPMAddPackage(
  NAME sourcepp
  GITHUB_REPOSITORY craftablescience/sourcepp
  GIT_TAG ffb7a52d27c0d83a2fb99bf10d3ef46bd545c431
)

# Vulkan-Headers vulkan-sdk-1.4.357.0, the same headers as the installed SDK (plan 0.1, D13).
CPMAddPackage(
  NAME Vulkan-Headers
  GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
  GIT_TAG e3b1eec08173d6b825cd3ac88c885a63b621504a
  OPTIONS "VULKAN_HEADERS_ENABLE_TESTS OFF" "VULKAN_HEADERS_ENABLE_INSTALL OFF"
          "VULKAN_HEADERS_ENABLE_MODULE OFF"
)

# volk vulkan-sdk-1.4.357.0 (Vulkan 1.4.357 loader, plan 0.1, D13). It takes its headers from the
# Vulkan-Headers pin above, not from a system Vulkan installation.
set(VOLK_PULL_IN_VULKAN OFF)
CPMAddPackage(
  NAME volk
  GITHUB_REPOSITORY zeux/volk
  GIT_TAG 776893306c5d3b22b6185b5d4a258b81d94572bf
  OPTIONS "VOLK_INSTALL OFF"
)
target_link_libraries(volk PUBLIC Vulkan::Headers)

# VulkanMemoryAllocator v3.3.0 (plan 0.1, D13). Header-only: exactly one translation unit defines
# VMA_IMPLEMENTATION (src/render/vma_impl.cpp).
CPMAddPackage(
  NAME VulkanMemoryAllocator
  GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
  GIT_TAG 1d8f600fd424278486eade7ed3e877c99f0846b1
)
# Third-party headers: keep the project's warning flags out of VMA's header.
target_include_directories(VulkanMemoryAllocator SYSTEM INTERFACE
                           "${VulkanMemoryAllocator_SOURCE_DIR}/include")

# Embree v4.4.1 (0.7.3, D3): CPU ray tracing for the baker's fallback when VK_KHR_ray_query is absent.
# MSVC needs 30+ minutes for each of Embree's AVX curve kernels (an estimated 11 hours for the library
# on the CI laptop), so Windows links Intel's prebuilt release of the same version instead: a shared
# embree4.dll with oneTBB tasking, pinned by hash. Everywhere else builds it from source.
option(APEX_EMBREE_PREBUILT "Link Intel's prebuilt Embree release instead of building it (Windows x64)" ${MSVC})
if(APEX_EMBREE_PREBUILT)
  if(NOT WIN32)
    message(FATAL_ERROR "APEX_EMBREE_PREBUILT is only available on Windows x64")
  endif()
  CPMAddPackage(
    NAME embree
    VERSION 4.4.1
    URL https://github.com/RenderKit/embree/releases/download/v4.4.1/embree-4.4.1.x64.windows.zip
    URL_HASH SHA256=5aa1fa3161a720f9c610b06c652b80f421cb2b1fe8221fdf92b74157336fdf8f
    DOWNLOAD_ONLY YES
  )
  find_package(embree 4.4.1 EXACT CONFIG REQUIRED PATHS "${embree_SOURCE_DIR}" NO_DEFAULT_PATH)
  set(APEX_EMBREE_DLLS "${embree_SOURCE_DIR}/bin/embree4.dll" "${embree_SOURCE_DIR}/bin/tbb12.dll"
                       "${embree_SOURCE_DIR}/bin/tbbmalloc.dll")
  # Every executable needs the DLLs beside it before it first runs (doctest discovery runs the test
  # binaries at build time), so copy them at configure time into each of our executables' folders.
  function(apex_copy_embree_dlls dir)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
      get_target_property(type ${target} TYPE)
      if(type STREQUAL "EXECUTABLE")
        get_target_property(out ${target} BINARY_DIR)
        file(COPY ${APEX_EMBREE_DLLS} DESTINATION "${out}")
      endif()
    endforeach()
    get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
      # Our own folders only: not dependencies, which may sit in the build tree's _deps.
      cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${sub}" ours)
      cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${sub}" built)
      if(ours AND NOT built)
        apex_copy_embree_dlls("${sub}")
      endif()
    endforeach()
  endfunction()
  cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL apex_copy_embree_dlls "${PROJECT_SOURCE_DIR}")
else()
  # Static, internal tasking (no TBB), no ISPC, no tutorials.
  set(EMBREE_STATIC_LIB ON CACHE BOOL "" FORCE)
  set(EMBREE_ISPC_SUPPORT OFF CACHE BOOL "" FORCE)
  set(EMBREE_TASKING_SYSTEM INTERNAL CACHE STRING "" FORCE)
  set(EMBREE_TUTORIALS OFF CACHE BOOL "" FORCE)
  set(EMBREE_INSTALL_DEPENDENCIES OFF CACHE BOOL "" FORCE)
  set(EMBREE_GEOMETRY_CURVE OFF CACHE BOOL "" FORCE)
  set(EMBREE_GEOMETRY_POINT OFF CACHE BOOL "" FORCE)
  set(EMBREE_GEOMETRY_SUBDIVISION OFF CACHE BOOL "" FORCE)
  set(EMBREE_GEOMETRY_USER OFF CACHE BOOL "" FORCE)
  set(EMBREE_GEOMETRY_GRID OFF CACHE BOOL "" FORCE)
  set(EMBREE_MAX_ISA AVX2 CACHE STRING "" FORCE)
  CPMAddPackage(
    NAME embree
    GITHUB_REPOSITORY RenderKit/embree
    GIT_TAG f590db83ef6559387df7f6d8725c34fb7acf851d
  )
  # Embree's ISA kernels are the build's heaviest compiles: no debug info (halves memory and time), at most
  # APEX_EMBREE_JOBS at once (2 suits a 15 GiB host; a dedicated build machine can raise it).
  set(APEX_EMBREE_JOBS 2 CACHE STRING "Max concurrent Embree kernel compiles (Ninja)")
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS apex_embree=${APEX_EMBREE_JOBS})
  foreach(target embree embree_sse42 embree_avx embree_avx2 sys tasking simd lexers math)
    if(TARGET ${target})
      if(NOT MSVC)
        target_compile_options(${target} PRIVATE -g0)
      endif()
      set_property(TARGET ${target} PROPERTY JOB_POOL_COMPILE apex_embree)
    endif()
  endforeach()
endif()

if(APEX_BUILD_APP)
  # Qt Multimedia (qt6-multimedia-dev) is required by apex_ui (root CMakeLists.txt).
  # Qt GuiPrivate (qt6-base-private-dev) for Wayland pointer lock. Qt-ADS was dropped in 0.2.12:
  # panels are area roles in the central split tree. Done in a function so Qt's directory-scope flag changes (it drops /EHsc on MSVC) don't leak.
  function(apex_find_qt_private)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets GuiPrivate)
  endfunction()
  apex_find_qt_private()
  set(QT_NO_PRIVATE_MODULE_WARNING ON)
endif()
