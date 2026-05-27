cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(einheit_core LANGUAGES C VERSION 0.1.0)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(einheit_core STATIC src/ec.c)

target_include_directories(einheit_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_options(einheit_core PRIVATE
  -Wall -Wextra -Wpedantic
)

if(NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_executable(test_ec test/test_ec.c)
  target_link_libraries(test_ec PRIVATE einheit_core)
  add_test(NAME test_ec COMMAND test_ec)
endif()
