/// @file zmq_local.h
/// @brief Local ZMQ transport over Unix-domain sockets (ipc://).
///
/// Used on-appliance when the CLI and daemon share a host.
/// Authentication is SO_PEERCRED on the daemon side; this transport
/// contributes no crypto. For remote sessions see zmq_remote.h.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_TRANSPORT_ZMQ_LOCAL_H_
#define INCLUDE_EINHEIT_CLI_TRANSPORT_ZMQ_LOCAL_H_

#include <expected>
#include <memory>
#include <string>

#include "einheit/cli/error.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::cli::transport {

/// Configuration for a local ZMQ transport.
struct ZmqLocalConfig {
  /// ipc:// path to the daemon's REP control socket.
  /// e.g. "ipc:///var/run/einheit/g.ctl".
  std::string control_endpoint;
  /// ipc:// path to the daemon's PUB event socket.
  /// e.g. "ipc:///var/run/einheit/g.pub".
  std::string event_endpoint;
};

/// Construct a not-yet-connected local ZMQ transport.
/// @param cfg Endpoints to bind to on Connect().
/// @returns Owned Transport, or TransportError.
auto NewZmqLocalTransport(const ZmqLocalConfig &cfg)
    -> std::expected<std::unique_ptr<Transport>,
                     Error<TransportError>>;

}  // namespace einheit::cli::transport

#endif  // INCLUDE_EINHEIT_CLI_TRANSPORT_ZMQ_LOCAL_H_
