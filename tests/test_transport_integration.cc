/// @file test_transport_integration.cc
/// @brief End-to-end integration test: CLI transport ↔ fake daemon.
// Copyright (c) 2026 Einheit Networks

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/transport/transport.h"
#include "einheit/cli/transport/zmq_local.h"

#include "tests/fake_daemon.h"

namespace einheit::cli::transport {

using testing::FakeDaemon;

TEST(TransportIntegration, RequestReplyOk) {
  FakeDaemon daemon([](const protocol::Request &req) {
    protocol::Response r;
    r.status = protocol::ResponseStatus::Ok;
    EXPECT_EQ(req.command, "show_status");
    return r;
  });

  ZmqLocalConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();

  auto tx = NewZmqLocalTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  protocol::Request req;
  req.id = "test-1";
  req.command = "show_status";
  auto resp = (*tx)->SendRequest(req, std::chrono::seconds(2));
  ASSERT_TRUE(resp.has_value()) << resp.error().message;
  EXPECT_EQ(resp->status, protocol::ResponseStatus::Ok);
  EXPECT_EQ(resp->id, "test-1");
}

TEST(TransportIntegration, ErrorPropagates) {
  FakeDaemon daemon([](const protocol::Request &) {
    protocol::Response r;
    r.status = protocol::ResponseStatus::Error;
    r.error = protocol::ResponseError{"policy_denied", "no way", "hm"};
    return r;
  });

  ZmqLocalConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();
  auto tx = NewZmqLocalTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  protocol::Request req;
  req.id = "test-2";
  req.command = "configure";
  auto resp = (*tx)->SendRequest(req, std::chrono::seconds(2));
  ASSERT_TRUE(resp.has_value());
  EXPECT_EQ(resp->status, protocol::ResponseStatus::Error);
  ASSERT_TRUE(resp->error.has_value());
  EXPECT_EQ(resp->error->code, "policy_denied");
}

TEST(TransportIntegration, SubscribeReceivesEvent) {
  FakeDaemon daemon([](const protocol::Request &) {
    return protocol::Response{};
  });

  ZmqLocalConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();
  auto tx = NewZmqLocalTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> received{false};
  std::string seen_topic;

  ASSERT_TRUE((*tx)
                  ->Subscribe("state.",
                              [&](const protocol::Event &ev) {
                                std::lock_guard<std::mutex> lk(mu);
                                seen_topic = ev.topic;
                                received.store(true);
                                cv.notify_all();
                              })
                  .has_value());

  // Give SUB time to register with the PUB; ZMQ is connection-async.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  protocol::Event ev;
  ev.topic = "state.tunnels.munich";
  ev.timestamp = "2026-04-20T00:00:00.000Z";
  // Publish a few times; PUB/SUB is lossy during handshake.
  for (int i = 0; i < 5 && !received.load(); ++i) {
    daemon.Publish("state.tunnels.munich", ev);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  std::unique_lock<std::mutex> lk(mu);
  cv.wait_for(lk, std::chrono::seconds(2),
              [&]() { return received.load(); });
  EXPECT_TRUE(received.load());
  EXPECT_EQ(seen_topic, "state.tunnels.munich");
}

}  // namespace einheit::cli::transport
