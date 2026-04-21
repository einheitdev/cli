# libcli_render — terminal caps + table renderer.

add_library(cli_render_obj OBJECT
  src/render/terminal_caps.cc
  src/render/table.cc
  src/render/sparkline.cc
  src/render/banner.cc
  src/render/confirm.cc
  src/render/pager.cc
  src/render/theme.cc
)

target_include_directories(cli_render_obj
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cli_render_obj
  PUBLIC
    ftxui::screen
    ftxui::dom
    yaml-cpp::yaml-cpp
)

add_library(cli_render STATIC
  $<TARGET_OBJECTS:cli_render_obj>
)

target_include_directories(cli_render
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(cli_render
  PUBLIC
    ftxui::screen
    ftxui::dom
    yaml-cpp::yaml-cpp
)
