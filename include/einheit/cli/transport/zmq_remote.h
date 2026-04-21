/// @file zmq_remote.h
/// @brief Remote ZMQ transport over TCP with CurveZMQ encryption.
///
/// Workstation-to-appliance transport. Client opens a DEALER to the
/// daemon's ROUTER (control) and a SUB to the daemon's XPUB (events).
/// CurveZMQ provides confidentiality + mutual authentication.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_TRANSPORT_ZMQ_REMOTE_H_
#define INCLUDE_EINHEIT_CLI_TRANSPORT_ZMQ_REMOTE_H_

#include <expected>
#include <memory>
#include <string>

#include "einheit/cli/error.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::cli::transport {

/// Configuration for a remote CurveZMQ transport.
struct ZmqRemoteConfig {
  /// tcp://host:port of the daemon's ROUTER control socket.
  std::string control_endpoint;
  /// tcp://host:port of the daemon's XPUB event socket.
  std::string event_endpoint;
  /// Z85-encoded Curve25519 public key of the daemon (server).
  /// Distributed out of band; pinned here by the client.
  std::string server_public_key;
  /// Z85-encoded client secret key; file on disk at mode 0600.
  std::string client_secret_key;
  /// Z85-encoded client public key (derived from secret; cached
  /// here to avoid re-deriving per request).
  std::string client_public_key;
};

/// Construct a not-yet-connected remote CurveZMQ transport.
/// @param cfg Endpoint + key material.
/// @returns Owned Transport, or TransportError.
auto NewZmqRemoteTransport(const ZmqRemoteConfig &cfg)
    -> std::expected<std::unique_ptr<Transport>,
                     Error<TransportError>>;

}  // namespace einheit::cli::transport

#endif  // INCLUDE_EINHEIT_CLI_TRANSPORT_ZMQ_REMOTE_H_
