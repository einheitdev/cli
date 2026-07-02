/// @file zmq_server.cc
/// @brief ZmqServer implementation — REP/PUB loop over a Runtime.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/zmq_server.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <zmq.hpp>

#include "einheit/cli/protocol/msgpack_codec.h"

namespace einheit::cli::confd {
namespace {

// Autogenerate a unique tmpdir ipc:// base when endpoints are blank.
auto TmpBase(const void *self) -> std::string {
  const auto base =
      std::filesystem::temp_directory_path() /
      std::format("einheit_confd_{}_{}", static_cast<int>(::getpid()),
                  reinterpret_cast<std::uintptr_t>(self));
  return base.string();
}

}  // namespace

struct ZmqServer::Impl {
  Runtime &runtime;
  std::string ctl_ep;
  std::string pub_ep;

  zmq::context_t ctx{1};
  zmq::socket_t rep{ctx, zmq::socket_type::rep};
  zmq::socket_t pub{ctx, zmq::socket_type::pub};
  std::mutex pub_mu;

  std::thread thread;
  std::atomic<bool> stop{false};

  explicit Impl(Runtime &rt) : runtime(rt) {}
};

namespace {

auto Loop(ZmqServer::Impl &d) -> void {
  zmq::pollitem_t item{static_cast<void *>(d.rep), 0, ZMQ_POLLIN, 0};
  while (!d.stop.load()) {
    if (zmq::poll(&item, 1, std::chrono::milliseconds(50)) <= 0) {
      continue;
    }
    zmq::message_t msg;
    auto got = d.rep.recv(msg, zmq::recv_flags::none);
    if (!got) continue;

    const auto *p = reinterpret_cast<const std::uint8_t *>(msg.data());
    auto req =
        protocol::DecodeRequest(std::span<const std::uint8_t>(p, msg.size()));
    protocol::Response resp;
    if (!req) {
      resp.status = protocol::ResponseStatus::Error;
      resp.error = protocol::ResponseError{"decode", req.error().message, ""};
    } else {
      resp = d.runtime.HandleRequest(*req);
    }
    auto bytes = protocol::EncodeResponse(resp);
    if (!bytes) continue;
    zmq::message_t out(bytes->data(), bytes->size());
    d.rep.send(out, zmq::send_flags::none);
  }
}

}  // namespace

ZmqServer::ZmqServer(Runtime &runtime, ZmqServerConfig cfg)
    : impl_(std::make_unique<Impl>(runtime)) {
  const auto base = TmpBase(this);
  impl_->ctl_ep = cfg.control_endpoint.empty()
                      ? std::format("ipc://{}.ctl", base)
                      : cfg.control_endpoint;
  impl_->pub_ep = cfg.event_endpoint.empty() ? std::format("ipc://{}.pub", base)
                                             : cfg.event_endpoint;

  impl_->rep.set(zmq::sockopt::linger, 0);
  impl_->pub.set(zmq::sockopt::linger, 0);
  impl_->rep.bind(impl_->ctl_ep);
  impl_->pub.bind(impl_->pub_ep);

  impl_->thread = std::thread([d = impl_.get()]() { Loop(*d); });
}

ZmqServer::~ZmqServer() {
  impl_->stop.store(true);
  if (impl_->thread.joinable()) impl_->thread.join();
}

auto ZmqServer::ControlEndpoint() const -> const std::string & {
  return impl_->ctl_ep;
}

auto ZmqServer::EventEndpoint() const -> const std::string & {
  return impl_->pub_ep;
}

auto ZmqServer::Publish(const protocol::Event &ev) -> void {
  auto body = protocol::EncodeEventBody(ev);
  if (!body) return;
  std::lock_guard<std::mutex> lk(impl_->pub_mu);
  try {
    zmq::message_t topic(ev.topic.data(), ev.topic.size());
    impl_->pub.send(topic, zmq::send_flags::sndmore);
    zmq::message_t frame(body->data(), body->size());
    impl_->pub.send(frame, zmq::send_flags::none);
  } catch (...) {
    // Shutting down or peer gone; events are best-effort.
  }
}

}  // namespace einheit::cli::confd
