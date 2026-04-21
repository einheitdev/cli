/// @file fake_daemon.h
/// @brief In-process ZMQ REP fixture for end-to-end transport tests.
///
/// Spins up REP and PUB sockets on temporary ipc:// paths, runs a
/// background thread that accepts Requests and passes them to a
/// caller-supplied handler. Used by integration tests that exercise
/// the transport + codec + dispatch stack without a real product
/// daemon.
// Copyright (c) 2026 Einheit Networks

#ifndef TESTS_FAKE_DAEMON_H_
#define TESTS_FAKE_DAEMON_H_

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <zmq.hpp>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/protocol/msgpack_codec.h"

namespace einheit::cli::testing {

using RequestHandler =
    std::function<protocol::Response(const protocol::Request &)>;

/// Background ZMQ REP server + PUB socket. Destructor joins.
class FakeDaemon {
 public:
  explicit FakeDaemon(RequestHandler handler)
      : handler_(std::move(handler)),
        ctx_(1),
        rep_(ctx_, zmq::socket_type::rep),
        pub_(ctx_, zmq::socket_type::pub) {
    const auto base =
        std::filesystem::temp_directory_path() /
        ("einheit_fake_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter_++));
    ctl_ep_ = "ipc://" + (base.string() + ".ctl");
    pub_ep_ = "ipc://" + (base.string() + ".pub");
    rep_.set(zmq::sockopt::linger, 0);
    pub_.set(zmq::sockopt::linger, 0);
    rep_.bind(ctl_ep_);
    pub_.bind(pub_ep_);
    thread_ = std::thread([this]() { Loop(); });
  }

  ~FakeDaemon() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }

  auto ControlEndpoint() const -> const std::string & { return ctl_ep_; }
  auto EventEndpoint() const -> const std::string & { return pub_ep_; }

  /// Publish a single event to any subscribed client.
  auto Publish(const std::string &topic,
               const protocol::Event &ev) -> void {
    auto body = protocol::EncodeEventBody(ev);
    if (!body) return;
    zmq::message_t t(topic.data(), topic.size());
    pub_.send(t, zmq::send_flags::sndmore);
    zmq::message_t b(body->data(), body->size());
    pub_.send(b, zmq::send_flags::none);
  }

 private:
  auto Loop() -> void {
    zmq::pollitem_t item{static_cast<void *>(rep_),
                         0, ZMQ_POLLIN, 0};
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

  static inline int counter_ = 0;
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

#endif  // TESTS_FAKE_DAEMON_H_
