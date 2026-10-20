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
