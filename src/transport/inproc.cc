/// @file inproc.cc
/// @brief In-process transport implementation.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/transport/inproc.h"

#include <memory>
#include <mutex>
#include <utility>

namespace einheit::cli::transport {

InProcTransport::InProcTransport(RequestFn handler)
    : handler_(std::move(handler)) {}

auto InProcTransport::Connect() -> std::expected<void, Error<TransportError>> {
  if (!handler_) {
    return std::unexpected(Error<TransportError>{
        TransportError::InvalidState, "no in-process handler bound"});
  }
  return {};
}

auto InProcTransport::Disconnect() -> void {}

auto InProcTransport::SendRequest(const protocol::Request &req,
                                  std::chrono::milliseconds)
    -> std::expected<protocol::Response, Error<TransportError>> {
  if (!handler_) {
    return std::unexpected(Error<TransportError>{
        TransportError::InvalidState, "no in-process handler bound"});
  }
  // Synchronous: the handler runs on the caller's thread. The timeout
  // is meaningless with no wire, so it is ignored.
  return handler_(req);
}

auto InProcTransport::Subscribe(const std::string &topic_prefix,
                                EventCallback cb)
    -> std::expected<void, Error<TransportError>> {
  std::lock_guard<std::mutex> lk(mu_);
  subscriptions_.emplace_back(topic_prefix, std::move(cb));
  return {};
}

auto InProcTransport::Unsubscribe(const std::string &topic_prefix)
    -> std::expected<void, Error<TransportError>> {
  std::lock_guard<std::mutex> lk(mu_);
  std::erase_if(subscriptions_,
                [&](const auto &s) { return s.first == topic_prefix; });
  return {};
}

auto InProcTransport::Publish(const protocol::Event &ev) -> void {
  std::vector<EventCallback> matched;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto &[prefix, cb] : subscriptions_) {
      if (ev.topic.rfind(prefix, 0) == 0 && cb) matched.push_back(cb);
    }
  }
  // Invoke outside the lock so a callback can (un)subscribe.
  for (const auto &cb : matched) cb(ev);
}

auto NewInProcTransport(RequestFn handler) -> std::unique_ptr<InProcTransport> {
  return std::make_unique<InProcTransport>(std::move(handler));
}

}  // namespace einheit::cli::transport
