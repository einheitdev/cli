/// @file zmq_local.cc
/// @brief Local ZMQ (ipc://) transport implementation.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/transport/zmq_local.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zmq.hpp>

#include "einheit/cli/protocol/msgpack_codec.h"

namespace einheit::cli::transport {
namespace {

auto MakeError(TransportError code, std::string message)
    -> Error<TransportError> {
  return Error<TransportError>{code, std::move(message)};
}

class ZmqLocalTransport : public Transport {
 public:
  // DEALER rather than REQ: REQ's lock-step state machine
  // wedges if a daemon restart drops an in-flight reply,
  // and DEALER is the canonical pair for the daemon's
  // ROUTER anyway. We preserve the `[empty][body]`
  // framing REQ used so the daemon side doesn't have to
  // special-case transports.
  explicit ZmqLocalTransport(ZmqLocalConfig cfg)
      : cfg_(std::move(cfg)),
        ctx_(1),
        ctrl_sock_(ctx_, zmq::socket_type::dealer) {}

  ~ZmqLocalTransport() override { Disconnect(); }

  auto Connect()
      -> std::expected<void, Error<TransportError>> override {
    try {
      ctrl_sock_.set(zmq::sockopt::linger, 0);
      ctrl_sock_.connect(cfg_.control_endpoint);
      connected_ = true;
      return {};
    } catch (const zmq::error_t &e) {
      return std::unexpected(
          MakeError(TransportError::ConnectFailed, e.what()));
    } catch (const std::exception &e) {
      return std::unexpected(
          MakeError(TransportError::ExceptionRaised, e.what()));
    }
  }

  auto Disconnect() -> void override {
    if (!connected_) return;
    event_stop_.store(true);
    if (event_thread_.joinable()) {
      event_thread_.join();
    }
    try {
      ctrl_sock_.close();
    } catch (...) {
    }
    connected_ = false;
  }

  auto SendRequest(const protocol::Request &req,
                   std::chrono::milliseconds timeout)
      -> std::expected<protocol::Response,
                       Error<TransportError>> override {
    if (!connected_) {
      return std::unexpected(MakeError(
          TransportError::InvalidState, "transport not connected"));
    }

    auto encoded = protocol::EncodeRequest(req);
    if (!encoded) {
      return std::unexpected(MakeError(
          TransportError::CodecError, encoded.error().message));
    }

    try {
      std::lock_guard<std::mutex> lk(ctrl_mu_);
      // DEALER doesn't auto-insert the empty delimiter
      // REQ had; the daemon's ROUTER expects
      // [identity][empty][body], so we send the empty
      // frame ourselves.
      zmq::message_t empty;
      ctrl_sock_.send(empty, zmq::send_flags::sndmore);
      zmq::message_t frame(encoded->data(), encoded->size());
      ctrl_sock_.send(frame, zmq::send_flags::none);

      zmq::pollitem_t item{static_cast<void *>(ctrl_sock_),
                           0, ZMQ_POLLIN, 0};
      const long ms = static_cast<long>(timeout.count());
      const int rc = zmq::poll(&item, 1, std::chrono::milliseconds{ms});
      if (rc <= 0) {
        return std::unexpected(
            MakeError(TransportError::Timeout, "request timed out"));
      }

      // Discard the matching empty delimiter before the
      // payload frame.
      zmq::message_t delim;
      auto got_delim =
          ctrl_sock_.recv(delim, zmq::recv_flags::none);
      if (!got_delim) {
        return std::unexpected(MakeError(
            TransportError::ReceiveFailed,
            "recv delimiter returned empty"));
      }
      zmq::message_t reply;
      auto got = ctrl_sock_.recv(reply, zmq::recv_flags::none);
      if (!got) {
        return std::unexpected(MakeError(
            TransportError::ReceiveFailed, "recv returned empty"));
      }

      const auto *p =
          reinterpret_cast<const std::uint8_t *>(reply.data());
      auto decoded = protocol::DecodeResponse(
          std::span<const std::uint8_t>(p, reply.size()));
      if (!decoded) {
        return std::unexpected(MakeError(
            TransportError::CodecError, decoded.error().message));
      }
      return *std::move(decoded);
    } catch (const zmq::error_t &e) {
      return std::unexpected(
          MakeError(TransportError::SendFailed, e.what()));
    } catch (const std::exception &e) {
      return std::unexpected(
          MakeError(TransportError::ExceptionRaised, e.what()));
    }
  }

  auto Subscribe(const std::string &topic_prefix, EventCallback cb)
      -> std::expected<void, Error<TransportError>> override {
    {
      std::lock_guard<std::mutex> lk(subs_mu_);
      subs_[topic_prefix] = std::move(cb);
    }
    EnsureEventThread();
    try {
      event_sub_->set(zmq::sockopt::subscribe, topic_prefix);
    } catch (const zmq::error_t &e) {
      return std::unexpected(
          MakeError(TransportError::ExceptionRaised, e.what()));
    }
    return {};
  }

  auto Unsubscribe(const std::string &topic_prefix)
      -> std::expected<void, Error<TransportError>> override {
    std::lock_guard<std::mutex> lk(subs_mu_);
    subs_.erase(topic_prefix);
    if (event_sub_) {
      try {
        event_sub_->set(zmq::sockopt::unsubscribe, topic_prefix);
      } catch (...) {
      }
    }
    return {};
  }

 private:
  auto EnsureEventThread() -> void {
    if (event_sub_) return;
    event_sub_ =
        std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::sub);
    event_sub_->connect(cfg_.event_endpoint);
    event_stop_.store(false);
    event_thread_ = std::thread([this]() { EventLoop(); });
  }

  auto EventLoop() -> void {
    while (!event_stop_.load()) {
      zmq::message_t topic_frame;
      zmq::message_t body_frame;
      zmq::pollitem_t item{static_cast<void *>(*event_sub_),
                           0, ZMQ_POLLIN, 0};
      if (zmq::poll(&item, 1, std::chrono::milliseconds(100)) <= 0) {
        continue;
      }
      try {
        auto got_topic =
            event_sub_->recv(topic_frame, zmq::recv_flags::none);
        auto got_body =
            event_sub_->recv(body_frame, zmq::recv_flags::none);
        if (!got_topic || !got_body) continue;

        std::string topic(static_cast<const char *>(topic_frame.data()),
                          topic_frame.size());
        const auto *p =
            reinterpret_cast<const std::uint8_t *>(body_frame.data());
        auto ev = protocol::DecodeEventBody(
            topic, std::span<const std::uint8_t>(p, body_frame.size()));
        if (!ev) continue;

        std::lock_guard<std::mutex> lk(subs_mu_);
        for (const auto &[prefix, cb] : subs_) {
          if (topic.rfind(prefix, 0) == 0) {
            cb(*ev);
          }
        }
      } catch (...) {
        // Best-effort; drop the frame and keep looping.
      }
    }
  }

  ZmqLocalConfig cfg_;
  zmq::context_t ctx_;
  zmq::socket_t ctrl_sock_;
  std::mutex ctrl_mu_;

  std::unique_ptr<zmq::socket_t> event_sub_;
  std::thread event_thread_;
  std::atomic<bool> event_stop_{false};
  std::mutex subs_mu_;
  std::unordered_map<std::string, EventCallback> subs_;

  bool connected_ = false;
};

}  // namespace

auto NewZmqLocalTransport(const ZmqLocalConfig &cfg)
    -> std::expected<std::unique_ptr<Transport>,
                     Error<TransportError>> {
  try {
    return std::unique_ptr<Transport>(new ZmqLocalTransport(cfg));
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(TransportError::ExceptionRaised, e.what()));
  }
}

}  // namespace einheit::cli::transport
