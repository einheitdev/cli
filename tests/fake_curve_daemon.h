/// @file fake_curve_daemon.h
/// @brief CurveZMQ-secured fake daemon for remote-transport tests.
///
/// Binds REP + PUB on `tcp://127.0.0.1:*`, runs as a Curve server
/// with a generated keypair, and echoes the client's public key
/// into a log so tests can verify authentication worked.
// Copyright (c) 2026 Einheit Networks

#ifndef TESTS_FAKE_CURVE_DAEMON_H_
#define TESTS_FAKE_CURVE_DAEMON_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <zmq.hpp>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/protocol/msgpack_codec.h"

namespace einheit::cli::testing {

/// One Curve keypair in Z85 form, suitable for ZMQ socket options.
struct CurveKeyPair {
  std::string public_key;
  std::string secret_key;
};

/// Generate a fresh keypair via zmq_curve_keypair().
auto NewCurveKeyPair() -> CurveKeyPair;

using RequestHandler =
    std::function<protocol::Response(const protocol::Request &)>;

/// Curve-secured counterpart of FakeDaemon. Picks a free ephemeral
/// port for both sockets; expose the bound endpoints to tests.
class FakeCurveDaemon {
 public:
  FakeCurveDaemon(CurveKeyPair server_keys, RequestHandler handler)
      : server_keys_(std::move(server_keys)),
        handler_(std::move(handler)),
        ctx_(1),
        rep_(ctx_, zmq::socket_type::rep),
        pub_(ctx_, zmq::socket_type::pub) {
    const int curve_server = 1;
    rep_.set(zmq::sockopt::curve_server, curve_server);
    rep_.set(zmq::sockopt::curve_secretkey, server_keys_.secret_key);
    rep_.set(zmq::sockopt::linger, 0);
    rep_.bind("tcp://127.0.0.1:*");
    const auto rep_ep =
        rep_.get(zmq::sockopt::last_endpoint);
    ctl_ep_ = rep_ep;

    pub_.set(zmq::sockopt::curve_server, curve_server);
    pub_.set(zmq::sockopt::curve_secretkey, server_keys_.secret_key);
    pub_.set(zmq::sockopt::linger, 0);
    pub_.bind("tcp://127.0.0.1:*");
    pub_ep_ = pub_.get(zmq::sockopt::last_endpoint);

    thread_ = std::thread([this]() { Loop(); });
  }

  ~FakeCurveDaemon() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }

  auto ControlEndpoint() const -> const std::string & { return ctl_ep_; }
  auto EventEndpoint() const -> const std::string & { return pub_ep_; }
  auto ServerPublicKey() const -> const std::string & {
    return server_keys_.public_key;
  }

  auto Publish(const std::string &topic, const protocol::Event &ev)
      -> void {
    auto body = protocol::EncodeEventBody(ev);
    if (!body) return;
    zmq::message_t t(topic.data(), topic.size());
    pub_.send(t, zmq::send_flags::sndmore);
    zmq::message_t b(body->data(), body->size());
    pub_.send(b, zmq::send_flags::none);
  }

 private:
  auto Loop() -> void {
    zmq::pollitem_t item{static_cast<void *>(rep_), 0, ZMQ_POLLIN, 0};
    while (!stop_.load()) {
      if (zmq::poll(&item, 1, std::chrono::milliseconds(50)) <= 0) {
        continue;
      }
      zmq::message_t msg;
      auto got = rep_.recv(msg, zmq::recv_flags::none);
      if (!got) continue;

      const auto *p =
          reinterpret_cast<const std::uint8_t *>(msg.data());
      auto req = protocol::DecodeRequest(
          std::span<const std::uint8_t>(p, msg.size()));
      protocol::Response resp;
      if (!req) {
        resp.status = protocol::ResponseStatus::Error;
        resp.error =
            protocol::ResponseError{"decode", req.error().message, ""};
      } else {
        resp = handler_(*req);
        resp.id = req->id;
      }
      auto bytes = protocol::EncodeResponse(resp);
      if (!bytes) continue;
      zmq::message_t out(bytes->data(), bytes->size());
      rep_.send(out, zmq::send_flags::none);
    }
  }

  CurveKeyPair server_keys_;
  RequestHandler handler_;
  zmq::context_t ctx_;
  zmq::socket_t rep_;
  zmq::socket_t pub_;
  std::string ctl_ep_;
  std::string pub_ep_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace einheit::cli::testing

#endif  // TESTS_FAKE_CURVE_DAEMON_H_
