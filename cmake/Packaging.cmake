# Install rules for release packaging (plan 0.1.10). tools/package.py drives
# `cmake --install` into a staging directory and then adds the Qt runtime and archives it.
# Included from the top-level CMakeLists.txt after the app block. Every rule is in the apex component:
# dependencies' own install rules (Embree's headers, man pages and CMake config) stay out of the packages.
include(GNUInstallDirs)

if(TARGET apex)
  if(WIN32)
    # Windows zip layout is flat: apex.exe next to the Qt DLLs that windeployqt adds.
    install(TARGETS apex RUNTIME DESTINATION . COMPONENT apex)
    install(FILES LICENSE THIRD_PARTY.md DESTINATION . COMPONENT apex)
    install(FILES third_party/ibm-plex-sans/LICENSE.txt DESTINATION . RENAME LICENSE-IBM-Plex-Sans.txt COMPONENT apex)
    if(APEX_EMBREE_PREBUILT)
      # The prebuilt Embree release (deps.cmake) is a DLL with oneTBB beside it.
      install(FILES ${APEX_EMBREE_DLLS} DESTINATION . COMPONENT apex)
      install(FILES "${embree_SOURCE_DIR}/share/doc/Embree_superbuild/LICENSE.txt" DESTINATION .
              RENAME LICENSE-Embree.txt COMPONENT apex)
      install(FILES "${embree_SOURCE_DIR}/share/doc/Embree_superbuild/third-party-programs-TBB.txt"
              DESTINATION . RENAME LICENSE-oneTBB.txt COMPONENT apex)
    endif()
  else()
    install(TARGETS apex RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT apex)
    install(FILES LICENSE THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT apex)
    install(FILES third_party/ibm-plex-sans/LICENSE.txt DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME LICENSE-IBM-Plex-Sans.txt COMPONENT apex)
  endif()
endif()

# ---- The plugin SDK (0.9.9, 0.9 D8): the `sdk` component ----
# Headers, the CMake package (the ApexPlugin package: apex::plugin_sdk, apex_add_plugin()), the example plugins'
# sources with the project file that builds them, and docs/plugins. No Apex source beyond that.
# `tools/package.py --sdk` stages this component and zips it into apex-plugin-sdk-<version>.zip, one archive for
# every platform. Paths are fixed (not GNUInstallDirs' lib64 and so on) because the archive is relocatable and
# platform neutral; ApexPluginConfig.cmake finds the headers relative to itself.
include(CMakePackageConfigHelpers)
file(STRINGS "${PROJECT_SOURCE_DIR}/include/apex/apex_plugin.h" _abi_major REGEX "^#define APEX_PLUGIN_ABI_MAJOR ")
file(STRINGS "${PROJECT_SOURCE_DIR}/include/apex/apex_plugin.h" _abi_minor REGEX "^#define APEX_PLUGIN_ABI_MINOR ")
string(REGEX REPLACE "^.* ([0-9]+)u?$" "\\1" _abi_major "${_abi_major}")
string(REGEX REPLACE "^.* ([0-9]+)u?$" "\\1" _abi_minor "${_abi_minor}")
# The package version is the ABI version: a request for ApexPlugin 1 matches any 1.x, never 2.
write_basic_package_version_file("${CMAKE_BINARY_DIR}/sdk/ApexPluginConfigVersion.cmake"
  VERSION ${_abi_major}.${_abi_minor}.0 COMPATIBILITY SameMajorVersion ARCH_INDEPENDENT)
install(FILES include/apex/apex_plugin.h include/apex/plugin.hpp DESTINATION include/apex COMPONENT sdk)
install(FILES cmake/sdk/ApexPluginConfig.cmake cmake/sdk/ApexPluginHelpers.cmake
              "${CMAKE_BINARY_DIR}/sdk/ApexPluginConfigVersion.cmake"
        DESTINATION lib/cmake/ApexPlugin COMPONENT sdk)
install(DIRECTORY examples/ DESTINATION share/apex-sdk/examples COMPONENT sdk)
install(DIRECTORY docs/plugins/ DESTINATION share/doc/apex-sdk COMPONENT sdk FILES_MATCHING PATTERN "*.md")
install(FILES LICENSE DESTINATION . COMPONENT sdk)
