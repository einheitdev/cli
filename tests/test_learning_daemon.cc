/// @file test_learning_daemon.cc
/// @brief Exercises the learning daemon's in-memory state machine
/// through the real wire protocol.
// Copyright (c) 2026 Einheit Networks

#include <chrono>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/learning_daemon.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/transport/zmq_local.h"

namespace einheit::cli::learning {
namespace {

auto BuildTransport(const LearningDaemon &d)
    -> std::unique_ptr<transport::Transport> {
  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = d.ControlEndpoint();
  cfg.event_endpoint = d.EventEndpoint();
  auto tx = transport::NewZmqLocalTransport(cfg);
  if (!tx) return nullptr;
  if (auto r = (*tx)->Connect(); !r) return nullptr;
  return std::move(*tx);
}

auto Send(transport::Transport &tx, protocol::Request req)
    -> protocol::Response {
  using namespace std::chrono_literals;
  auto r = tx.SendRequest(req, 2s);
  EXPECT_TRUE(r.has_value());
  return *r;
}

auto DataAsString(const protocol::Response &r) -> std::string {
  return std::string(r.data.begin(), r.data.end());
}

}  // namespace

TEST(LearningDaemon, ConfigureSetCommitArc) {
  LearningDaemon d;
  auto tx = BuildTransport(d);
  ASSERT_NE(tx, nullptr);

  // configure → session id echoed in data
  protocol::Request req;
  req.command = "configure";
  auto resp = Send(*tx, req);
  EXPECT_EQ(resp.status, protocol::ResponseStatus::Ok);
  const auto sid = DataAsString(resp);
  EXPECT_FALSE(sid.empty());

  // set hostname demo
  req = {};
  req.command = "set";
  req.session_id = sid;
  req.args = {"hostname", "demo"};
  resp = Send(*tx, req);
  EXPECT_EQ(resp.status, protocol::ResponseStatus::Ok);

  // commit
  req = {};
  req.command = "commit";
  req.session_id = sid;
  resp = Send(*tx, req);
  EXPECT_EQ(resp.status, protocol::ResponseStatus::Ok);
  EXPECT_NE(DataAsString(resp).find("commit_id=1"),
            std::string::npos);

  // show_config now reflects the running state
  req = {};
  req.command = "show_config";
  resp = Send(*tx, req);
  EXPECT_EQ(resp.status, protocol::ResponseStatus::Ok);
  EXPECT_NE(DataAsString(resp).find("hostname=demo"),
            std::string::npos);
}

TEST(LearningDaemon, SetWithoutSessionErrors) {
  LearningDaemon d;
  auto tx = BuildTransport(d);
  ASSERT_NE(tx, nullptr);

  protocol::Request req;
  req.command = "set";
  req.args = {"hostname", "demo"};
  auto resp = Send(*tx, req);
  EXPECT_EQ(resp.status, protocol::ResponseStatus::Error);
  ASSERT_TRUE(resp.error.has_value());
  EXPECT_EQ(resp.error->code, "no_session");
}

TEST(LearningDaemon, RollbackCandidateClears) {
  LearningDaemon d;
  auto tx = BuildTransport(d);
  ASSERT_NE(tx, nullptr);

  protocol::Request req;
  req.command = "configure";
  auto resp = Send(*tx, req);
  const auto sid = DataAsString(resp);

  req = {};
  req.command = "set";
  req.session_id = sid;
  req.args = {"hostname", "ephemeral"};
  Send(*tx, req);

  req = {};
  req.command = "rollback";
  req.args = {"candidate"};
  resp = Send(*tx, req);
  EXPECT_EQ(resp.status, protocol::ResponseStatus::Ok);

  // show_config should be empty now.
  req = {};
  req.command = "show_config";
  resp = Send(*tx, req);
  EXPECT_EQ(DataAsString(resp), "");
}

TEST(LearningDaemon, TraceSinkReceivesLines) {
  std::ostringstream trace;
  LearningDaemon d(&trace);
  auto tx = BuildTransport(d);
  ASSERT_NE(tx, nullptr);

  protocol::Request req;
  req.command = "show_status";
  Send(*tx, req);
  // Give the trace thread a moment to flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto body = trace.str();
  EXPECT_NE(body.find("→ show_status"), std::string::npos);
  EXPECT_NE(body.find("← ok"), std::string::npos);
}

TEST(LearningDaemon, UnknownCommandRejected) {
  LearningDaemon d;
  auto tx = BuildTransport(d);
  ASSERT_NE(tx, nullptr);

  protocol::Request req;
  req.command = "totally_bogus";
  auto resp = Send(*tx, req);
  EXPECT_EQ(resp.status, protocol::ResponseStatus::Error);
  ASSERT_TRUE(resp.error.has_value());
  EXPECT_EQ(resp.error->code, "unknown");
}

}  // namespace einheit::cli::learning
