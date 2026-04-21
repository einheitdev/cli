# Third-party dependencies for einheit-cli.
#
# Libraries:
#   CLI11            (BSD-3-Clause)   — argument parsing
#   cppzmq           (MIT)            — header-only ZMQ binding
#   msgpack-cxx      (Boost)          — MessagePack serialization
#   FTXUI            (MIT)            — terminal rendering
#   spdlog           (MIT, shared)    — logging
#   yaml-cpp         (MIT)            — YAML schema/config parsing
#   GoogleTest       (BSD-3)          — unit test framework
#
# System packages (find_package):
#   libzmq (libzmq3-dev)
#   libsodium (libsodium-dev)  — provides CurveZMQ backend
#   yaml-cpp (libyaml-cpp-dev)
#   readline (libreadline-dev)

include(FetchContent)

set(third_party_install_root "third_party")

# ----- libzmq (system package) ----------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(libzmq REQUIRED IMPORTED_TARGET libzmq)

# ----- libsodium (system package; CurveZMQ crypto backend) ------------------
pkg_check_modules(libsodium REQUIRED IMPORTED_TARGET libsodium)

# ----- cppzmq (header-only) -------------------------------------------------
if(NOT TARGET cppzmq)
  find_package(cppzmq QUIET)
endif()
if(NOT TARGET cppzmq)
  FetchContent_Declare(cppzmq
    GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
    GIT_TAG v4.10.0
    GIT_SHALLOW TRUE
  )
  set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(cppzmq)
endif()

# ----- readline removed — replxx (fetched below) is the line editor -------

# ----- yaml-cpp (system package preferred) ----------------------------------
find_package(yaml-cpp QUIET)
if(NOT TARGET yaml-cpp::yaml-cpp AND NOT TARGET yaml-cpp)
  FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
    GIT_SHALLOW TRUE
  )
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(yaml-cpp)
endif()
if(TARGET yaml-cpp AND NOT TARGET yaml-cpp::yaml-cpp)
  add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp)
endif()

# ----- msgpack-cxx ----------------------------------------------------------
FetchContent_Declare(msgpack-cxx
  GIT_REPOSITORY https://github.com/msgpack/msgpack-c.git
  GIT_TAG cpp-6.1.1
  GIT_SHALLOW TRUE
)
set(MSGPACK_CXX20 OFF CACHE BOOL "" FORCE)
set(MSGPACK_USE_BOOST OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MSGPACK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(msgpack-cxx)

# ----- spdlog ---------------------------------------------------------------
set(SPDLOG_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.16.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# ----- CLI11 ----------------------------------------------------------------
FetchContent_Declare(cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.6.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cli11)
if(TARGET CLI11 AND NOT TARGET CLI11::CLI11)
  add_library(CLI11::CLI11 ALIAS CLI11)
endif()

# ----- FTXUI ----------------------------------------------------------------
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v6.1.9
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ftxui)

# ----- replxx (interactive line editing) ------------------------------------
set(REPLXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(REPLXX_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(replxx
  GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
  GIT_TAG release-0.0.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(replxx)

# ----- GoogleTest (for tests) -----------------------------------------------
if(EINHEIT_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
