/// @file inproc.h
/// @brief In-process transport — the embedded single-binary path.
///
/// The confd runtime is a library that runs either standalone behind
/// ZMQ (client CLI + daemon) or embedded in-process (single binary,
/// direct-to-hardware). This transport is the embedded half: SendRequest
/// calls a handler (the runtime's HandleRequest) directly, with no wire.
/// Because both cases go through the same Transport interface and the
/// same Runtime::HandleRequest, "direct to hardware, no daemon" and
/// "client + daemon" are one code path, not two.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_TRANSPORT_INPROC_H_
#define INCLUDE_EINHEIT_CLI_TRANSPORT_INPROC_H_

#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "einheit/cli/error.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::cli::transport {

/// Synchronous request handler — typically bound to a confd
/// Runtime::HandleRequest.
using RequestFn = std::function<protocol::Response(const protocol::Request &)>;

/// A Transport that dispatches requests to an in-process handler.
/// Thread-safe for concurrent SendRequest / Publish.
class InProcTransport : public Transport {
 public:
  /// Construct over a request handler. The handler must outlive the
  /// transport (bind it to a Runtime that outlives this).
  /// @param handler Invoked for each SendRequest.
  explicit InProcTransport(RequestFn handler);

  auto Connect() -> std::expected<void, Error<TransportError>> override;
  auto Disconnect() -> void override;
  auto SendRequest(const protocol::Request &req,
                   std::chrono::milliseconds timeout)
      -> std::expected<protocol::Response, Error<TransportError>> override;
  auto Subscribe(const std::string &topic_prefix, EventCallback cb)
      -> std::expected<void, Error<TransportError>> override;
  auto Unsubscribe(const std::string &topic_prefix)
      -> std::expected<void, Error<TransportError>> override;

  /// Deliver an event to every subscription whose prefix matches the
  /// topic. Lets an embedded event source (e.g. a metrics publisher)
  /// feed `watch` in-process, mirroring the ZMQ PUB channel.
  /// @param ev Event to deliver.
  auto Publish(const protocol::Event &ev) -> void;

 private:
  RequestFn handler_;
  std::mutex mu_;
  std::vector<std::pair<std::string, EventCallback>> subscriptions_;
};

/// Construct an in-process transport over a handler.
/// @param handler Request handler (bound to Runtime::HandleRequest).
/// @returns Owned transport.
auto NewInProcTransport(RequestFn handler) -> std::unique_ptr<InProcTransport>;

}  // namespace einheit::cli::transport

#endif  // INCLUDE_EINHEIT_CLI_TRANSPORT_INPROC_H_
