include(GoogleTest)

function(einheit_add_test name)
  add_executable(${name} ${name}.cc)
  target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(${name}
    PRIVATE
      einheit_cli
      GTest::gtest
      GTest::gtest_main
  )
  gtest_discover_tests(${name})
endfunction()

einheit_add_test(test_envelope_roundtrip)
einheit_add_test(test_command_tree)
einheit_add_test(test_terminal_caps)
einheit_add_test(test_schema)
einheit_add_test(test_transport_integration)
einheit_add_test(test_globals)
einheit_add_test(test_session_lifecycle)
einheit_add_test(test_table_render)
einheit_add_test(test_sparkline)
einheit_add_test(test_oneshot)
einheit_add_test(test_fuzzy)
einheit_add_test(test_history_aliases)
einheit_add_test(test_watch)
einheit_add_test(test_net_parse)
einheit_add_test(test_codec_resilience)
einheit_add_test(test_target_config)
einheit_add_test(test_help)
einheit_add_test(test_curve_keys)
einheit_add_test(test_completions)
einheit_add_test(test_workstation_state)
einheit_add_test(test_shell_escape)
einheit_add_test(test_format_output)
einheit_add_test(test_learning_daemon)
einheit_add_test(test_theme)

# Adapter contract test links the shipped example adapter to assert
# the template stays well-formed.
add_executable(test_adapter_contract test_adapter_contract.cc)
target_include_directories(test_adapter_contract
  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_adapter_contract
  PRIVATE einheit_cli einheit_adapter_example
          GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_adapter_contract)

# Remote transport test links in the Curve-enabled fake daemon.
add_executable(test_remote_transport
  test_remote_transport.cc
  fake_curve_daemon.cc
)
target_include_directories(test_remote_transport
  PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_remote_transport
  PRIVATE einheit_cli GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_remote_transport)
