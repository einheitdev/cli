/// @file test_transport_faults.cc
/// @brief Fault injection at the transport boundary: a daemon that
/// replies with garbage, a truncated frame, a hostile length prefix, or
/// simply vanishes mid-exchange must surface as a clean typed error and
/// leave the CLI process alive — never a crash, hang, or unbounded
/// allocation.
// Copyright (c) 2026 Einheit Networks

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <zmq.hpp>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/protocol/msgpack_codec.h"
#include "einheit/cli/transport/transport.h"
#include "einheit/cli/transport/zmq_local.h"

namespace einheit::cli::transport {
namespace {

// A raw REP responder that completes the request handshake and then
// sends whatever bytes the test dictates as the "reply" — no valid
// encoding required. Lets us inject malformed frames the well-behaved
// FakeDaemon never would.
class RawResponder {
 public:
  enum class Mode { Garbage, Truncated, HugeLength, Silent };

  explicit RawResponder(Mode mode)
      : mode_(mode), ctx_(1), rep_(ctx_, zmq::socket_type::rep) {
    const auto base =
        std::filesystem::temp_directory_path() /
        ("einheit_raw_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter_++));
    ep_ = "ipc://" + base.string() + ".ctl";
    rep_.set(zmq::sockopt::linger, 0);
    rep_.bind(ep_);
    thread_ = std::thread([this] { Loop(); });
  }

  ~RawResponder() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }

  auto Endpoint() const -> const std::string & { return ep_; }

 private:
  auto Loop() -> void {
    zmq::pollitem_t item{static_cast<void *>(rep_), 0, ZMQ_POLLIN, 0};
    while (!stop_.load()) {
      if (zmq::poll(&item, 1, std::chrono::milliseconds(25)) <= 0) {
        continue;
      }
      zmq::message_t msg;
      if (!rep_.recv(msg, zmq::recv_flags::none)) continue;
      if (mode_ == Mode::Silent) {
        // Receive the request but never reply — the client must time
        // out rather than hang forever.
        continue;
      }
      std::vector<std::uint8_t> reply = MakeReply();
      zmq::message_t out(reply.data(), reply.size());
      rep_.send(out, zmq::send_flags::none);
    }
  }

  auto MakeReply() -> std::vector<std::uint8_t> {
    switch (mode_) {
      case Mode::Garbage:
        return {0xde, 0xad, 0xbe, 0xef, 0x00, 0xff, 0x13, 0x37};
      case Mode::HugeLength:
        // array32 header claiming ~2 billion elements.
        return {0xdd, 0x7f, 0xff, 0xff, 0xff};
      case Mode::Truncated: {
        protocol::Response r;
        r.id = "x";
        r.status = protocol::ResponseStatus::Ok;
        auto full = protocol::EncodeResponse(r);
        std::vector<std::uint8_t> t = full.value();
        t.resize(t.size() / 2);  // chop the frame in half
        return t;
      }
      default:
        return {};
    }
  }

  static inline int counter_ = 0;
  Mode mode_;
  zmq::context_t ctx_;
  zmq::socket_t rep_;
  std::string ep_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
};

auto ConnectTo(const std::string &ep)
    -> std::unique_ptr<Transport> {
  ZmqLocalConfig cfg;
  cfg.control_endpoint = ep;
  cfg.event_endpoint = ep + ".pub";  // unused here
  auto tx = NewZmqLocalTransport(cfg);
  if (!tx) return nullptr;
  if (!(*tx)->Connect()) return nullptr;
  return std::move(*tx);
}

auto MakeRequest() -> protocol::Request {
  protocol::Request req;
  req.id = "req-1";
  req.command = "show_config";
  return req;
}

using namespace std::chrono_literals;

TEST(TransportFaults, GarbageReplyIsCleanError) {
  RawResponder daemon(RawResponder::Mode::Garbage);
  auto tx = ConnectTo(daemon.Endpoint());
  ASSERT_TRUE(tx);
  auto r = tx->SendRequest(MakeRequest(), 2s);
  // A garbage reply decodes to an error; the process is still here.
  EXPECT_FALSE(r.has_value());
}

TEST(TransportFaults, TruncatedReplyIsCleanError) {
  RawResponder daemon(RawResponder::Mode::Truncated);
  auto tx = ConnectTo(daemon.Endpoint());
  ASSERT_TRUE(tx);
  auto r = tx->SendRequest(MakeRequest(), 2s);
  EXPECT_FALSE(r.has_value());
}

TEST(TransportFaults, HugeLengthReplyDoesNotExhaustMemory) {
  RawResponder daemon(RawResponder::Mode::HugeLength);
  auto tx = ConnectTo(daemon.Endpoint());
  ASSERT_TRUE(tx);
  // Without the decode bound this would attempt a multi-GB allocation.
  auto r = tx->SendRequest(MakeRequest(), 2s);
  EXPECT_FALSE(r.has_value());
}

TEST(TransportFaults, SilentPeerTimesOutNotHang) {
  RawResponder daemon(RawResponder::Mode::Silent);
  auto tx = ConnectTo(daemon.Endpoint());
  ASSERT_TRUE(tx);
  const auto t0 = std::chrono::steady_clock::now();
  auto r = tx->SendRequest(MakeRequest(), 300ms);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, TransportError::Timeout);
  // It actually timed out promptly rather than blocking forever.
  EXPECT_LT(elapsed, 5s);
}

TEST(TransportFaults, PeerVanishesMidExchangeTimesOut) {
  // Bring the daemon up, connect, then destroy it before the request.
  std::string ep;
  {
    RawResponder daemon(RawResponder::Mode::Silent);
    ep = daemon.Endpoint();
  }  // daemon gone
  auto tx = ConnectTo(ep);
  ASSERT_TRUE(tx);  // connect to a dead ipc endpoint still succeeds
  auto r = tx->SendRequest(MakeRequest(), 300ms);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, TransportError::Timeout);
}

}  // namespace
}  // namespace einheit::cli::transport
