/// @file zmq_remote.cc
/// @brief Remote CurveZMQ transport over TCP.
///
/// Sets CURVE_PUBLICKEY / CURVE_SECRETKEY / CURVE_SERVERKEY on every
/// socket before connect, then runs the same REQ/SUB loop as the
/// local transport. libzmq handles Noise-style handshake and
/// AEAD-encrypts every subsequent frame.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/transport/zmq_remote.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <exception>
#include <mutex>
#include <span>
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

// Apply Curve client-side keys to a socket. Returns an error if the
// configured keys look wrong before we even try to connect.
auto ApplyClientCurveKeys(zmq::socket_t &sock,
                          const ZmqRemoteConfig &cfg)
    -> std::expected<void, Error<TransportError>> {
  if (cfg.server_public_key.empty() ||
      cfg.client_secret_key.empty() ||
      cfg.client_public_key.empty()) {
    return std::unexpected(MakeError(
        TransportError::AuthFailed, "curve keys not configured"));
  }
  try {
    sock.set(zmq::sockopt::curve_serverkey, cfg.server_public_key);
    sock.set(zmq::sockopt::curve_publickey, cfg.client_public_key);
    sock.set(zmq::sockopt::curve_secretkey, cfg.client_secret_key);
  } catch (const zmq::error_t &e) {
    return std::unexpected(
        MakeError(TransportError::AuthFailed, e.what()));
  }
  return {};
}

class ZmqRemoteTransport : public Transport {
 public:
  // DEALER rather than REQ — same rationale as the local
  // transport: REQ's state machine can't recover from a
  // dropped reply after a daemon restart, and DEALER is
  // the canonical pair for ROUTER. Framing preserved so
  // the daemon side stays transport-agnostic.
  explicit ZmqRemoteTransport(ZmqRemoteConfig cfg)
      : cfg_(std::move(cfg)),
        ctx_(1),
        ctrl_sock_(ctx_, zmq::socket_type::dealer) {}

  ~ZmqRemoteTransport() override { Disconnect(); }

  auto Connect()
      -> std::expected<void, Error<TransportError>> override {
    if (auto r = ApplyClientCurveKeys(ctrl_sock_, cfg_); !r) {
      return std::unexpected(r.error());
    }
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
    if (event_thread_.joinable()) event_thread_.join();
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
      // DEALER→ROUTER framing: empty delimiter ahead of
      // the payload, matching what REQ used to insert
      // automatically.
      zmq::message_t empty;
      ctrl_sock_.send(empty, zmq::send_flags::sndmore);
      zmq::message_t frame(encoded->data(), encoded->size());
      ctrl_sock_.send(frame, zmq::send_flags::none);

      zmq::pollitem_t item{static_cast<void *>(ctrl_sock_),
                           0, ZMQ_POLLIN, 0};
      const long ms = static_cast<long>(timeout.count());
      const int rc =
          zmq::poll(&item, 1, std::chrono::milliseconds{ms});
      if (rc <= 0) {
        return std::unexpected(
            MakeError(TransportError::Timeout, "request timed out"));
      }

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
    if (auto r = EnsureEventThread(); !r) {
      return std::unexpected(r.error());
    }
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
  auto EnsureEventThread()
      -> std::expected<void, Error<TransportError>> {
    if (event_sub_) return {};
    event_sub_ =
        std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::sub);
    if (auto r = ApplyClientCurveKeys(*event_sub_, cfg_); !r) {
      event_sub_.reset();
      return std::unexpected(r.error());
    }
    try {
      event_sub_->connect(cfg_.event_endpoint);
    } catch (const zmq::error_t &e) {
      event_sub_.reset();
      return std::unexpected(
          MakeError(TransportError::ConnectFailed, e.what()));
    }
    event_stop_.store(false);
    event_thread_ = std::thread([this]() { EventLoop(); });
    return {};
  }

  auto EventLoop() -> void {
    while (!event_stop_.load()) {
      zmq::pollitem_t item{static_cast<void *>(*event_sub_),
                           0, ZMQ_POLLIN, 0};
      if (zmq::poll(&item, 1, std::chrono::milliseconds(100)) <= 0) {
        continue;
      }
      try {
        zmq::message_t topic_frame, body_frame;
        auto gt =
            event_sub_->recv(topic_frame, zmq::recv_flags::none);
        auto gb =
            event_sub_->recv(body_frame, zmq::recv_flags::none);
        if (!gt || !gb) continue;

        std::string topic(static_cast<const char *>(topic_frame.data()),
                          topic_frame.size());
        const auto *p =
            reinterpret_cast<const std::uint8_t *>(body_frame.data());
        auto ev = protocol::DecodeEventBody(
            topic, std::span<const std::uint8_t>(p, body_frame.size()));
        if (!ev) continue;

        std::lock_guard<std::mutex> lk(subs_mu_);
        for (const auto &[prefix, cb] : subs_) {
          if (topic.rfind(prefix, 0) == 0) cb(*ev);
        }
      } catch (...) {
      }
    }
  }

  ZmqRemoteConfig cfg_;
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

auto NewZmqRemoteTransport(const ZmqRemoteConfig &cfg)
    -> std::expected<std::unique_ptr<Transport>,
                     Error<TransportError>> {
  try {
    return std::unique_ptr<Transport>(new ZmqRemoteTransport(cfg));
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(TransportError::ExceptionRaised, e.what()));
  }
}

}  // namespace einheit::cli::transport
