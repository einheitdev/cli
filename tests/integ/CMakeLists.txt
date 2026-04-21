include(GoogleTest)

# Tiny standalone test daemon, built as a separate binary.
add_executable(minid minid.cc)
target_link_libraries(minid PRIVATE cli_protocol cppzmq
                                     PkgConfig::libzmq)

add_executable(test_end_to_end test_end_to_end.cc)
target_include_directories(test_end_to_end
  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_end_to_end
  PRIVATE einheit_cli GTest::gtest GTest::gtest_main)
# The test needs the built einheit binary + minid.
add_dependencies(test_end_to_end einheit minid)
gtest_discover_tests(test_end_to_end
  PROPERTIES ENVIRONMENT "EINHEIT_BUILD_DIR=${CMAKE_BINARY_DIR}")
