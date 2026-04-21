add_library(einheit_adapter_example STATIC
  src/adapter.cc
)

target_include_directories(einheit_adapter_example
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(einheit_adapter_example
  PUBLIC
    einheit_cli
    yaml-cpp::yaml-cpp
)
