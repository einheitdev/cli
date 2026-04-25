# libcli_core — schema, command tree, shell, auth/audit, history,
# aliases. The headline framework target adapters and binaries link
# against.

add_library(cli_core_obj OBJECT
  src/schema.cc
  src/command_tree.cc
  src/globals.cc
  src/session.cc
  src/shell.cc
  src/watch.cc
  src/shell_escape.cc
  src/auth.cc
  src/audit.cc
  src/adapter_contract.cc
  src/curve_keys.cc
  src/fuzzy.cc
  src/net_parse.cc
  src/target_config.cc
  src/workstation_state.cc
  src/history.cc
  src/aliases.cc
  src/line_reader.cc
  src/learning_daemon.cc
  src/locked_sandbox.cc
)

target_include_directories(cli_core_obj
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cli_core_obj
  PUBLIC
    cli_protocol
    cli_transport
    cli_render
    yaml-cpp::yaml-cpp
    spdlog::spdlog
    CLI11::CLI11
    replxx::replxx
)

add_library(einheit_cli STATIC
  $<TARGET_OBJECTS:cli_core_obj>
)

target_include_directories(einheit_cli
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(einheit_cli
  PUBLIC
    cli_protocol
    cli_transport
    cli_render
    yaml-cpp::yaml-cpp
    spdlog::spdlog
    CLI11::CLI11
    replxx::replxx
)
