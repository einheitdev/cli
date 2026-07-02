/// @file test_confd_transports.cc
/// @brief Proves the confd runtime is ONE library exercised BOTH ways:
/// the identical engine-driven lifecycle runs embedded (in-process
/// transport) and standalone (ZMQ REP client), and the fake hardware
/// ends up in the same state. No forked lightweight-vs-managed code.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/confd/memory_backend.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/confd/zmq_server.h"
#include "einheit/cli/engine.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/session.h"
#include "einheit/cli/transport/inproc.h"
#include "einheit/cli/transport/zmq_local.h"

namespace einheit::cli::confd {
namespace {

auto EmptySchema() -> std::shared_ptr<const schema::Schema> {
  return std::make_shared<const schema::Schema>();
}

// Run configure → set hostname → commit through the reusable engine
// against whatever transport is handed in. Returns the transport used
// so the caller can keep it alive. This is the SAME code for both
// transports — the whole point of the test.
auto DriveLifecycle(transport::Transport &tx) -> void {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  Session session;

  engine::Context ctx;
  ctx.tx = &tx;
  ctx.session = &session;
  ctx.caller.user = "root";
  ctx.caller.role = RoleGate::AdminOnly;

  auto run = [&](const std::vector<std::string> &tokens) {
    auto parsed = Parse(tree, tokens, RoleGate::AdminOnly);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
    auto out = engine::Execute(ctx, *parsed);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->wire, engine::WireStatus::Ok);
    if (out->response) {
      EXPECT_EQ(out->response->status, protocol::ResponseStatus::Ok)
          << (out->response->error ? out->response->error->message : "");
    }
  };

  run({"configure"});
  EXPECT_TRUE(session.in_configure);
  run({"set", "hostname", "shared-path"});
  run({"commit"});
  EXPECT_FALSE(session.in_configure);
}

TEST(ConfdTransports, EmbeddedInProcess) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);

  auto tx = transport::NewInProcTransport(
      [&](const protocol::Request &req) { return rt.HandleRequest(req); });
  ASSERT_TRUE(tx->Connect().has_value());

  DriveLifecycle(*tx);

  // The commit reached the fake hardware through the embedded path.
  EXPECT_EQ(backend.DeviceState().at("hostname"), "shared-path");
  EXPECT_EQ(rt.HistorySize(), 1u);
}

TEST(ConfdTransports, StandaloneBehindZmq) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  ZmqServer server(rt);  // binds tmpdir ipc:// endpoints

  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = server.ControlEndpoint();
  cfg.event_endpoint = server.EventEndpoint();
  auto tx = transport::NewZmqLocalTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  DriveLifecycle(**tx);

  // The commit reached the same fake hardware through the wire path.
  EXPECT_EQ(backend.DeviceState().at("hostname"), "shared-path");
  EXPECT_EQ(rt.HistorySize(), 1u);
}

TEST(ConfdTransports, BothPathsProduceIdenticalRunningConfig) {
  MemoryBackend embedded_hw(EmptySchema());
  Runtime embedded_rt(embedded_hw);
  auto in_tx = transport::NewInProcTransport([&](const protocol::Request &req) {
    return embedded_rt.HandleRequest(req);
  });
  ASSERT_TRUE(in_tx->Connect().has_value());
  DriveLifecycle(*in_tx);

  MemoryBackend wire_hw(EmptySchema());
  Runtime wire_rt(wire_hw);
  ZmqServer server(wire_rt);
  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = server.ControlEndpoint();
  cfg.event_endpoint = server.EventEndpoint();
  auto wire_tx = transport::NewZmqLocalTransport(cfg);
  ASSERT_TRUE(wire_tx.has_value());
  ASSERT_TRUE((*wire_tx)->Connect().has_value());
  DriveLifecycle(**wire_tx);

  EXPECT_EQ(embedded_hw.DeviceState(), wire_hw.DeviceState());
}

}  // namespace
}  // namespace einheit::cli::confd
