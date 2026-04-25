# Install rules for einheit-cli.
#
# - /usr/bin/einheit                binary
# - /usr/include/einheit/cli/...    public framework headers (devel)
# - /usr/lib/<arch>/libeinheit_cli.a   (archive, for adapter linking; devel)
# - /usr/share/einheit/schema/      shared schema fragments
# - /usr/share/doc/einheit-cli/     CLAUDE.md + README + LICENCE
#
# When einheit is bundled as a sub-build (e.g. inside hyper-derp's
# deb), the devel artifacts (headers + .a) are dead weight — only the
# runtime binary needs to ship.  Set EINHEIT_INSTALL_DEVEL=OFF in the
# parent project to skip those rules.

include(GNUInstallDirs)

option(EINHEIT_INSTALL_DEVEL
  "Install development artifacts (headers + static archives)" ON)

install(TARGETS einheit
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT runtime
)

if(EINHEIT_INSTALL_DEVEL)
  install(TARGETS einheit_cli cli_core_obj cli_protocol cli_transport cli_render
    EXPORT einheit-cli-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT devel
    OPTIONAL
  )

  install(
    DIRECTORY include/einheit
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT devel
    FILES_MATCHING PATTERN "*.h"
  )
endif()

# Shared schema fragments (common_auth, common_audit, common_logging).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/schema")
  install(
    DIRECTORY schema/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/einheit/schema
    COMPONENT runtime
    FILES_MATCHING PATTERN "*.yaml"
  )
endif()

if(EINHEIT_INSTALL_DEVEL)
  install(
    FILES README.md CLAUDE.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    COMPONENT devel
    OPTIONAL
  )
endif()
