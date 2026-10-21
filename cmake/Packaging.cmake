# Install rules for release packaging (plan 0.1.10). tools/package.py drives
# `cmake --install` into a staging directory and then adds the Qt runtime and archives it.
# Included from the top-level CMakeLists.txt after the app block.
include(GNUInstallDirs)

if(TARGET apex)
  if(WIN32)
    # Windows zip layout is flat: apex.exe next to the Qt DLLs that windeployqt adds.
    install(TARGETS apex RUNTIME DESTINATION .)
    install(FILES LICENSE THIRD_PARTY.md DESTINATION .)
  else()
    install(TARGETS apex RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES LICENSE THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
  endif()
endif()
