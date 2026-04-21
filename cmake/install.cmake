# Install rules for einheit-cli.
#
# - /usr/bin/einheit                binary
# - /usr/include/einheit/cli/...    public framework headers
# - /usr/lib/<arch>/libeinheit_cli.a   (archive, for adapter linking)
# - /usr/share/einheit/schema/      shared schema fragments
# - /usr/share/doc/einheit-cli/     CLAUDE.md + README + LICENCE

include(GNUInstallDirs)

install(TARGETS einheit
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT runtime
)

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

# Shared schema fragments (common_auth, common_audit, common_logging).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/schema")
  install(
    DIRECTORY schema/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/einheit/schema
    COMPONENT runtime
    FILES_MATCHING PATTERN "*.yaml"
  )
endif()

install(
  FILES README.md CLAUDE.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
  COMPONENT runtime
  OPTIONAL
)
