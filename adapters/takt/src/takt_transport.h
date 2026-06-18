/// @file takt_transport.h
/// @brief ZMQ transport that speaks takt-service's JSON
/// protocol instead of the framework's msgpack envelopes.
// Copyright (c) 2026 Einheit Networks

#ifndef ADAPTERS_TAKT_TAKT_TRANSPORT_H_
#define ADAPTERS_TAKT_TAKT_TRANSPORT_H_

#include <expected>
#include <memory>
#include <string>

#include "einheit/cli/error.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::adapters::takt {

/// Configuration for the takt ZMQ transport.
struct TaktTransportConfig {
  /// Control socket endpoint.
  std::string control_endpoint;
  /// Event socket endpoint.
  std::string event_endpoint;
};

/// Create a ZMQ transport that translates between the
/// framework's Request/Response envelopes and
/// takt-service's JSON-over-ZMQ protocol.
auto MakeTaktTransport(TaktTransportConfig cfg)
    -> std::expected<
        std::unique_ptr<cli::transport::Transport>,
        cli::Error<cli::transport::TransportError>>;

}  // namespace einheit::adapters::takt

#endif  // ADAPTERS_TAKT_TAKT_TRANSPORT_H_
