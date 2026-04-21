# Top-level `einheit` binary. Links the framework core + an adapter
# and provides main(). Packaged binary is named `einheit` on the
# appliance regardless of which adapter is bundled.

add_executable(einheit
  binaries/einheit/src/main.cc
)

target_link_libraries(einheit
  PRIVATE
    einheit_cli
    einheit_adapter_example
)

set_target_properties(einheit PROPERTIES
  OUTPUT_NAME einheit
)

install(TARGETS einheit RUNTIME DESTINATION bin)
