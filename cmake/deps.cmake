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

# SourcePP v2026.9.11: only the libraries 0.1 needs (vpkpp, steampp, vtfpp, toolpp).
set(SOURCEPP_LIBS_START_ENABLED OFF CACHE BOOL "" FORCE)
set(SOURCEPP_USE_VPKPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_STEAMPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_VTFPP ON CACHE BOOL "" FORCE)
set(SOURCEPP_USE_TOOLPP ON CACHE BOOL "" FORCE)
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

if(APEX_BUILD_APP)
  # Qt Advanced Docking System 4.4.1 (LGPL-2.1; plan 0.1, D16), static. Needs Qt6 (found by the
  # caller) and qt6-base-private-dev. Side panels only: never host native Vulkan windows (D15).
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(QT_NO_PRIVATE_MODULE_WARNING ON)
  CPMAddPackage(
    NAME qtads
    GITHUB_REPOSITORY githubuser0xFFFF/Qt-Advanced-Docking-System
    GIT_TAG 258340842056271ec207f9b112ac5efc45d9b5f5
    DOWNLOAD_ONLY YES
  )
  if(qtads_ADDED)
    add_subdirectory(${qtads_SOURCE_DIR} ${qtads_BINARY_DIR} EXCLUDE_FROM_ALL SYSTEM)
  endif()
endif()
