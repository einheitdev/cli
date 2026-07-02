/// @file adapter.h
/// @brief takt CLI adapter — connects to takt-service
/// via ZMQ for pipeline orchestration commands.
// Copyright (c) 2026 Einheit Networks

#ifndef ADAPTERS_TAKT_ADAPTER_H_
#define ADAPTERS_TAKT_ADAPTER_H_

#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/error.h"
#include "einheit/cli/schema.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::adapters::takt {

/// Construct a takt ProductAdapter.
/// @returns Owned ProductAdapter.
auto NewTaktAdapter()
    -> std::unique_ptr<einheit::cli::ProductAdapter>;

/// Create a ZMQ transport for takt-service. Speaks JSON
/// instead of the framework's msgpack envelopes.
/// @param control Control socket endpoint.
/// @param event Event socket endpoint.
/// @returns Transport or error.
auto NewTaktTransport(
    const std::string &control,
    const std::string &event)
    -> std::expected<
        std::unique_ptr<einheit::cli::transport::Transport>,
        einheit::cli::Error<
            einheit::cli::transport::TransportError>>;

}  // namespace einheit::adapters::takt

#endif  // ADAPTERS_TAKT_ADAPTER_H_
