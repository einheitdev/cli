/// @file minid.cc
/// @brief Tiny standalone test daemon — implements the wire
/// protocol just enough that `einheit` can round-trip against it
/// without the in-process LearningDaemon. Reads a control
/// endpoint from argv[1], binds REP + PUB, handles a handful of
/// commands, exits on SIGTERM.
///
/// Wire protocol matches LearningDaemon's minimal model: echoes
/// commit_id=1 from `show_commits`, an "ok" status from everything
/// else. Intended for CI end-to-end tests, not for any operator.
// Copyright (c) 2026 Einheit Networks

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <zmq.hpp>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/protocol/msgpack_codec.h"

namespace {
std::atomic<bool> stop{false};
}

auto main(int argc, char **argv) -> int {
  if (argc < 2) {
    std::cerr << "usage: minid <ipc-path>\n";
    return 2;
  }
  const std::string ctl = argv[1];
  const std::string pub = ctl + ".pub";

  std::signal(SIGTERM,
              [](int) { stop.store(true); });
  std::signal(SIGINT,
              [](int) { stop.store(true); });

  zmq::context_t ctx(1);
  zmq::socket_t rep(ctx, zmq::socket_type::rep);
  zmq::socket_t pub_s(ctx, zmq::socket_type::pub);
  rep.set(zmq::sockopt::linger, 0);
  pub_s.set(zmq::sockopt::linger, 0);
  rep.bind(ctl);
  pub_s.bind(pub);

  zmq::pollitem_t item{static_cast<void *>(rep), 0, ZMQ_POLLIN, 0};
  while (!stop.load()) {
    if (zmq::poll(&item, 1, std::chrono::milliseconds(50)) <= 0) {
      continue;
    }
    zmq::message_t msg;
    if (!rep.recv(msg, zmq::recv_flags::none)) continue;
    const auto *p =
        reinterpret_cast<const std::uint8_t *>(msg.data());
    auto req = einheit::cli::protocol::DecodeRequest(
        std::span<const std::uint8_t>(p, msg.size()));
    einheit::cli::protocol::Response r;
    if (!req) {
      r.status = einheit::cli::protocol::ResponseStatus::Error;
      r.error = einheit::cli::protocol::ResponseError{
          "decode", req.error().message, ""};
    } else {
      r.id = req->id;
      r.status = einheit::cli::protocol::ResponseStatus::Ok;
      if (req->command == "show_commits") {
        const std::string body = "commit_id=1\n";
        r.data.assign(body.begin(), body.end());
      } else if (req->command == "show_status") {
        const std::string body = "ok=1\n";
        r.data.assign(body.begin(), body.end());
      }
    }
    auto bytes = einheit::cli::protocol::EncodeResponse(r);
    if (!bytes) continue;
    zmq::message_t out(bytes->data(), bytes->size());
    rep.send(out, zmq::send_flags::none);
  }
  return 0;
}
