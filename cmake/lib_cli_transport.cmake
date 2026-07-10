# libcli_transport — abstract Transport + oneshot + in-proc, plus the
# ZMQ local/remote transports unless EINHEIT_NO_ZMQ is set (embedded
# in-proc consumers cross-compile without libzmq/libsodium).

set(_cli_transport_sources
  src/transport/oneshot.cc
  src/transport/inproc.cc
)
if(NOT EINHEIT_NO_ZMQ)
  list(APPEND _cli_transport_sources
    src/transport/zmq_local.cc
    src/transport/zmq_remote.cc
  )
endif()

add_library(cli_transport_obj OBJECT
  ${_cli_transport_sources}
)

target_include_directories(cli_transport_obj
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cli_transport_obj
  PUBLIC
    cli_protocol
)

add_library(cli_transport STATIC
  $<TARGET_OBJECTS:cli_transport_obj>
)

target_include_directories(cli_transport
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cli_transport
  PUBLIC
    cli_protocol
)

if(NOT EINHEIT_NO_ZMQ)
  target_link_libraries(cli_transport_obj
    PUBLIC
      cppzmq
      PkgConfig::libzmq
      PkgConfig::libsodium
  )
  target_link_libraries(cli_transport
    PUBLIC
      cppzmq
      PkgConfig::libzmq
      PkgConfig::libsodium
  )
endif()
