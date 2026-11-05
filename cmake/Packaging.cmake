# Install rules for release packaging (plan 0.1.10). tools/package.py drives
# `cmake --install` into a staging directory and then adds the Qt runtime and archives it.
# Included from the top-level CMakeLists.txt after the app block.
include(GNUInstallDirs)

if(TARGET apex)
  if(WIN32)
    # Windows zip layout is flat: apex.exe next to the Qt DLLs that windeployqt adds.
    install(TARGETS apex RUNTIME DESTINATION .)
    install(FILES LICENSE THIRD_PARTY.md DESTINATION .)
    install(FILES third_party/ibm-plex-sans/LICENSE.txt DESTINATION . RENAME LICENSE-IBM-Plex-Sans.txt)
    if(APEX_EMBREE_PREBUILT)
      # The prebuilt Embree release (deps.cmake) is a DLL with oneTBB beside it.
      install(FILES ${APEX_EMBREE_DLLS} DESTINATION .)
      install(FILES "${embree_SOURCE_DIR}/share/doc/Embree_superbuild/LICENSE.txt" DESTINATION .
              RENAME LICENSE-Embree.txt)
      install(FILES "${embree_SOURCE_DIR}/share/doc/Embree_superbuild/third-party-programs-TBB.txt"
              DESTINATION . RENAME LICENSE-oneTBB.txt)
    endif()
  else()
    install(TARGETS apex RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES LICENSE THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(FILES third_party/ibm-plex-sans/LICENSE.txt DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME LICENSE-IBM-Plex-Sans.txt)
  endif()
endif()
