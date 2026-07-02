add_library(einheit_adapter_takt STATIC
  src/adapter.cc
  src/takt_transport.cc
)

target_include_directories(einheit_adapter_takt
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(einheit_adapter_takt
  PUBLIC
    einheit_cli
    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
    cppzmq
)
