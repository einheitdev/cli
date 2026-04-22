add_library(einheit_adapter_hd_relay STATIC
  src/adapter.cc
)

target_include_directories(einheit_adapter_hd_relay
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(einheit_adapter_hd_relay
  PUBLIC
    einheit_cli
    yaml-cpp::yaml-cpp
)
