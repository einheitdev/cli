/// @file test_remote_transport.cc
/// @brief End-to-end CurveZMQ transport: DEALER/REQ client with
/// pinned server key talks to a REP server with the matching secret.
// Copyright (c) 2026 Einheit Networks

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/transport/transport.h"
#include "einheit/cli/transport/zmq_remote.h"

#include "tests/fake_curve_daemon.h"

namespace einheit::cli::transport {

using testing::FakeCurveDaemon;
using testing::NewCurveKeyPair;

TEST(RemoteTransport, RequestReplyOk) {
  auto server_keys = NewCurveKeyPair();
  auto client_keys = NewCurveKeyPair();

  FakeCurveDaemon daemon(server_keys,
                         [](const protocol::Request &req) {
                           protocol::Response r;
                           EXPECT_EQ(req.command, "show_status");
                           r.status = protocol::ResponseStatus::Ok;
                           return r;
                         });

  ZmqRemoteConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();
  cfg.server_public_key = daemon.ServerPublicKey();
  cfg.client_secret_key = client_keys.secret_key;
  cfg.client_public_key = client_keys.public_key;

  auto tx = NewZmqRemoteTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  protocol::Request req;
  req.id = "rem-1";
  req.command = "show_status";
  auto resp = (*tx)->SendRequest(req, std::chrono::seconds(3));
  ASSERT_TRUE(resp.has_value()) << resp.error().message;
  EXPECT_EQ(resp->status, protocol::ResponseStatus::Ok);
  EXPECT_EQ(resp->id, "rem-1");
}

TEST(RemoteTransport, WrongServerKeyFailsHandshake) {
  auto server_keys = NewCurveKeyPair();
  auto decoy_keys = NewCurveKeyPair();
  auto client_keys = NewCurveKeyPair();

  FakeCurveDaemon daemon(server_keys,
                         [](const protocol::Request &) {
                           return protocol::Response{};
                         });

  // Pin the wrong server key; libzmq drops frames silently. Request
  // should time out rather than succeed.
  ZmqRemoteConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();
  cfg.server_public_key = decoy_keys.public_key;
  cfg.client_secret_key = client_keys.secret_key;
  cfg.client_public_key = client_keys.public_key;

  auto tx = NewZmqRemoteTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  protocol::Request req;
  req.id = "rem-2";
  req.command = "show_status";
  auto resp = (*tx)->SendRequest(req, std::chrono::milliseconds(500));
  ASSERT_FALSE(resp.has_value());
  EXPECT_EQ(resp.error().code, TransportError::Timeout);
}

TEST(RemoteTransport, SubscribeReceivesEvent) {
  auto server_keys = NewCurveKeyPair();
  auto client_keys = NewCurveKeyPair();

  FakeCurveDaemon daemon(server_keys,
                         [](const protocol::Request &) {
                           return protocol::Response{};
                         });

  ZmqRemoteConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();
  cfg.server_public_key = daemon.ServerPublicKey();
  cfg.client_secret_key = client_keys.secret_key;
  cfg.client_public_key = client_keys.public_key;

  auto tx = NewZmqRemoteTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> received{false};
  std::string seen;

  ASSERT_TRUE((*tx)
                  ->Subscribe("state.",
                              [&](const protocol::Event &ev) {
                                std::lock_guard<std::mutex> lk(mu);
                                seen = ev.topic;
                                received.store(true);
                                cv.notify_all();
                              })
                  .has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  protocol::Event ev;
  ev.topic = "state.tunnels.frankfurt";
  ev.timestamp = "2026-04-20T00:00:00.000Z";
  for (int i = 0; i < 6 && !received.load(); ++i) {
    daemon.Publish(ev.topic, ev);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::unique_lock<std::mutex> lk(mu);
  cv.wait_for(lk, std::chrono::seconds(3),
              [&]() { return received.load(); });
  EXPECT_TRUE(received.load());
  EXPECT_EQ(seen, "state.tunnels.frankfurt");
}

TEST(RemoteTransport, MissingKeysRejectedAtConnect) {
  ZmqRemoteConfig cfg;
  cfg.control_endpoint = "tcp://127.0.0.1:1";
  cfg.event_endpoint = "tcp://127.0.0.1:2";
  // No keys set.

  auto tx = NewZmqRemoteTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  auto r = (*tx)->Connect();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, TransportError::AuthFailed);
}

}  // namespace einheit::cli::transport
