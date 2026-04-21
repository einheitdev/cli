/// @file transport.h
/// @brief Abstract wire transport between CLI and product daemon.
///
/// Two implementations live alongside: zmq_local (ipc:// Unix
/// socket + SO_PEERCRED auth) and zmq_remote (tcp:// + CurveZMQ
/// auth). Adapters see only this interface.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_TRANSPORT_TRANSPORT_H_
#define INCLUDE_EINHEIT_CLI_TRANSPORT_TRANSPORT_H_

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "einheit/cli/error.h"
#include "einheit/cli/protocol/envelope.h"

namespace einheit::cli::transport {

/// Errors surfaced by the transport layer.
enum class TransportError {
  /// Could not connect to the configured endpoint.
  ConnectFailed,
  /// Authentication failed (bad Curve key, peer not authorised).
  AuthFailed,
  /// Wire send failed (peer gone, socket closed).
  SendFailed,
  /// Wire receive failed (truncated frame, protocol violation).
  ReceiveFailed,
  /// Request did not complete before the configured timeout.
  Timeout,
  /// Codec rejected the response payload.
  CodecError,
  /// Underlying ZMQ library raised an exception.
  ExceptionRaised,
  /// Operation called in an invalid state (e.g. not connected).
  InvalidState,
};

/// Callback invoked for each Event that arrives on a subscribed
/// topic. Runs on the transport's event thread; must not block.
using EventCallback =
    std::function<void(const protocol::Event &)>;

/// Abstract base for every transport implementation.
class Transport {
 public:
  virtual ~Transport() = default;

  /// Open the control + event sockets.
  /// @returns void on success, or TransportError.
  virtual auto Connect()
      -> std::expected<void, Error<TransportError>> = 0;

  /// Tear down sockets. Idempotent.
  virtual auto Disconnect() -> void = 0;

  /// Send one Request and wait for the matching Response.
  /// @param req Request to send.
  /// @param timeout Maximum time to wait for a reply.
  /// @returns Response on success, or TransportError.
  virtual auto SendRequest(
      const protocol::Request &req,
      std::chrono::milliseconds timeout)
      -> std::expected<protocol::Response, Error<TransportError>> = 0;

  /// Subscribe to a ZMQ PUB/SUB topic prefix on the event channel.
  /// @param topic_prefix Hierarchical dotted prefix ("state.").
  /// @param cb Callback invoked per Event.
  /// @returns void on success, or TransportError.
  virtual auto Subscribe(const std::string &topic_prefix,
                         EventCallback cb)
      -> std::expected<void, Error<TransportError>> = 0;

  /// Remove a previously-installed subscription.
  /// @param topic_prefix The exact prefix passed to Subscribe.
  virtual auto Unsubscribe(const std::string &topic_prefix)
      -> std::expected<void, Error<TransportError>> = 0;
};

}  // namespace einheit::cli::transport

#endif  // INCLUDE_EINHEIT_CLI_TRANSPORT_TRANSPORT_H_
