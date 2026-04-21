/// @file adapter_contract.cc
/// @brief Structural adapter validator — "if you declared it, you
/// must render it" is the invariant.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/adapter_contract.h"

#include <exception>
#include <format>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/table.h"
#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::contract {
namespace {

auto MakeError(ContractError code, std::string message)
    -> Error<ContractError> {
  return Error<ContractError>{code, std::move(message)};
}

}  // namespace

auto ValidateAdapter(const ProductAdapter &adapter)
    -> std::expected<ContractReport, Error<ContractError>> {
  ContractReport report;
  std::unordered_set<std::string> wire_commands;

  // Sink renderer: ANSI disabled so adapters can't accidentally
  // depend on terminal width when rendering an empty payload.
  std::ostringstream sink;
  render::TerminalCaps caps;
  caps.force_plain = true;
  caps.width = 80;
  render::Renderer renderer(sink, caps);

  for (const auto &spec : adapter.Commands()) {
    if (spec.path.empty()) {
      return std::unexpected(MakeError(
          ContractError::InvalidSpec,
          "spec has empty path"));
    }
    // A wire_command is required UNLESS the spec declares event
    // topics — watch-only adapter commands (e.g. `metrics`) exist
    // solely to carry subscribe metadata and never cross the RPC
    // wire.
    if (spec.wire_command.empty() &&
        adapter.EventTopicsFor(spec).empty()) {
      return std::unexpected(MakeError(
          ContractError::InvalidSpec,
          std::format("spec '{}' has neither wire_command nor "
                      "event topics",
                      spec.path)));
    }
    if (!spec.wire_command.empty() &&
        !wire_commands.insert(spec.wire_command).second) {
      return std::unexpected(MakeError(
          ContractError::DuplicateWireCommand,
          std::format("two specs share wire_command '{}'",
                      spec.wire_command)));
    }

    protocol::Response empty_ok;
    empty_ok.status = protocol::ResponseStatus::Ok;
    try {
      adapter.RenderResponse(spec, empty_ok, renderer);
    } catch (const std::exception &e) {
      return std::unexpected(MakeError(
          ContractError::RenderResponseThrew,
          std::format("RenderResponse for '{}' threw: {}",
                      spec.path, e.what())));
    } catch (...) {
      return std::unexpected(MakeError(
          ContractError::RenderResponseThrew,
          std::format("RenderResponse for '{}' threw unknown",
                      spec.path)));
    }
    ++report.commands_checked;

    for (const auto &topic : adapter.EventTopicsFor(spec)) {
      protocol::Event empty_event;
      empty_event.topic = topic;
      try {
        adapter.RenderEvent(topic, empty_event, renderer);
      } catch (const std::exception &e) {
        return std::unexpected(MakeError(
            ContractError::RenderEventThrew,
            std::format("RenderEvent for '{}' on '{}' threw: {}",
                        spec.path, topic, e.what())));
      } catch (...) {
        return std::unexpected(MakeError(
            ContractError::RenderEventThrew,
            std::format("RenderEvent for '{}' on '{}' threw unknown",
                        spec.path, topic)));
      }
      ++report.topics_checked;
    }
  }
  return report;
}

}  // namespace einheit::cli::contract
