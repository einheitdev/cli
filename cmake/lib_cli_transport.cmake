# libcli_transport — abstract Transport + ZMQ local/remote + oneshot.

add_library(cli_transport_obj OBJECT
  src/transport/zmq_local.cc
  src/transport/zmq_remote.cc
  src/transport/oneshot.cc
)

target_include_directories(cli_transport_obj
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cli_transport_obj
  PUBLIC
    cli_protocol
    cppzmq
    PkgConfig::libzmq
    PkgConfig::libsodium
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
    cppzmq
    PkgConfig::libzmq
    PkgConfig::libsodium
)
