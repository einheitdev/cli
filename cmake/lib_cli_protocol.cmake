# libcli_protocol — wire envelope + MessagePack codec.

add_library(cli_protocol_obj OBJECT
  src/protocol/msgpack_codec.cc
)

target_include_directories(cli_protocol_obj
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cli_protocol_obj
  PUBLIC
    msgpack-cxx
)

add_library(cli_protocol STATIC
  $<TARGET_OBJECTS:cli_protocol_obj>
)

target_include_directories(cli_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cli_protocol
  PUBLIC
    msgpack-cxx
)
