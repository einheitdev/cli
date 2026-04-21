/// @file adapter_contract.h
/// @brief Structural validation for ProductAdapter implementations.
///
/// Every CommandSpec an adapter publishes must have a RenderResponse
/// handler that doesn't throw on an empty Response. Every declared
/// watch topic must come with a RenderEvent handler that doesn't
/// throw on an empty Event. Run this from each adapter's test suite
/// to catch "adapter advertised X but forgot to implement the
/// renderer" regressions before shipping.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_ADAPTER_CONTRACT_H_
#define INCLUDE_EINHEIT_CLI_ADAPTER_CONTRACT_H_

#include <expected>
#include <string>
#include <vector>

#include "einheit/cli/adapter.h"
#include "einheit/cli/error.h"

namespace einheit::cli::contract {

/// Errors raised when an adapter violates the framework contract.
enum class ContractError {
  /// A CommandSpec's RenderResponse threw on a minimal input.
  RenderResponseThrew,
  /// A watch-topic subscription's RenderEvent threw.
  RenderEventThrew,
  /// Two CommandSpecs share the same wire_command.
  DuplicateWireCommand,
  /// CommandSpec has empty path or wire_command.
  InvalidSpec,
};

/// Report returned on success. Collects counts of what was checked.
struct ContractReport {
  std::size_t commands_checked = 0;
  std::size_t topics_checked = 0;
};

/// Walk every CommandSpec the adapter declares, invoke its
/// RenderResponse with an empty OK Response, and — for any command
/// that declares event topics — invoke RenderEvent with an empty
/// Event. Any exception or invalid metadata is surfaced as an
/// Error<ContractError> with the offending spec's path in the
/// message.
/// @param adapter Adapter to check.
/// @returns Report on success, ContractError on first violation.
auto ValidateAdapter(const ProductAdapter &adapter)
    -> std::expected<ContractReport, Error<ContractError>>;

}  // namespace einheit::cli::contract

#endif  // INCLUDE_EINHEIT_CLI_ADAPTER_CONTRACT_H_
