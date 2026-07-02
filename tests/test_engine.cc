/// @file test_engine.cc
/// @brief Drives the reusable command engine directly: session
/// threading, one-path session gating, audit emission, and wire-error
/// classification — the behaviour every front-end (shell, UI, confd)
/// now shares.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "einheit/cli/audit.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/engine.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/session.h"
#include "einheit/cli/transport/zmq_local.h"
#include "tests/fake_daemon.h"

namespace einheit::cli::engine {
namespace {

using einheit::cli::testing::FakeDaemon;

// Stateful daemon that implements just enough of the lifecycle to
// exercise the engine's session threading.
struct DaemonState {
  std::mutex mu;
  std::optional<std::string> active_session;
  std::vector<std::pair<std::string, std::string>> sets;
  int commits = 0;
};

auto MakeDaemon(DaemonState &state) -> FakeDaemon {
  return FakeDaemon([&](const protocol::Request &req) {
    std::lock_guard<std::mutex> lk(state.mu);
    protocol::Response r;
    if (req.command == "configure") {
      state.active_session = "sess-7";
      r.data.assign(state.active_session->begin(), state.active_session->end());
    } else if (req.command == "set") {
      if (!state.active_session || !req.session_id ||
          *req.session_id != *state.active_session) {
        r.status = protocol::ResponseStatus::Error;
        r.error =
            protocol::ResponseError{"no_session", "set without configure", ""};
      } else if (req.args.size() >= 2) {
        state.sets.emplace_back(req.args[0], req.args[1]);
      }
    } else if (req.command == "commit") {
      ++state.commits;
      state.active_session.reset();
    }
    return r;
  });
}

// Connect a transport to a fake daemon endpoint.
auto Connect(const std::string &ctl, const std::string &pub)
    -> std::unique_ptr<transport::Transport> {
  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = ctl;
  cfg.event_endpoint = pub;
  auto tx = transport::NewZmqLocalTransport(cfg);
  if (!tx) return nullptr;
  if (auto r = (*tx)->Connect(); !r) return nullptr;
  return std::move(*tx);
}

auto ParseIn(const CommandTree &tree, const std::vector<std::string> &tokens)
    -> ParsedCommand {
  auto p = Parse(tree, tokens, RoleGate::AdminOnly);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message);
  return *p;
}

struct Fixture {
  DaemonState state;
  FakeDaemon daemon{MakeDaemon(state)};
  CommandTree tree;
  Session session;
  std::unique_ptr<transport::Transport> tx;
  std::vector<audit::Record> audit_log;

  Fixture() {
    (void)RegisterGlobals(tree);
    tx = Connect(daemon.ControlEndpoint(), daemon.EventEndpoint());
  }

  auto Ctx() -> Context {
    Context ctx;
    ctx.tx = tx.get();
    ctx.session = &session;
    ctx.caller.user = "root";
    ctx.caller.role = RoleGate::AdminOnly;
    ctx.audit = [this](const audit::Record &r) { audit_log.push_back(r); };
    return ctx;
  }
};

TEST(Engine, ConfigureSetCommitThreadsSession) {
  Fixture f;
  ASSERT_TRUE(f.tx);

  auto ctx = f.Ctx();
  EXPECT_FALSE(f.session.in_configure);

  auto configure = ParseIn(f.tree, {"configure"});
  auto r1 = Execute(ctx, configure);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->wire, WireStatus::Ok);
  EXPECT_TRUE(f.session.in_configure);
  EXPECT_EQ(f.session.session_id.value_or(""), "sess-7");

  // `set` carries the session id issued by configure.
  auto set = ParseIn(f.tree, {"set", "hostname", "demo"});
  auto r2 = Execute(ctx, set);
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2->response.has_value());
  EXPECT_EQ(r2->response->status, protocol::ResponseStatus::Ok);
  {
    std::lock_guard<std::mutex> lk(f.state.mu);
    ASSERT_EQ(f.state.sets.size(), 1u);
    EXPECT_EQ(f.state.sets[0].first, "hostname");
    EXPECT_EQ(f.state.sets[0].second, "demo");
  }

  auto commit = ParseIn(f.tree, {"commit"});
  auto r3 = Execute(ctx, commit);
  ASSERT_TRUE(r3.has_value());
  EXPECT_FALSE(f.session.in_configure);
  EXPECT_FALSE(f.session.session_id.has_value());
  {
    std::lock_guard<std::mutex> lk(f.state.mu);
    EXPECT_EQ(f.state.commits, 1);
  }
}

TEST(Engine, SetWithoutConfigureRejectedBeforeWire) {
  Fixture f;
  ASSERT_TRUE(f.tx);
  auto ctx = f.Ctx();

  auto set = ParseIn(f.tree, {"set", "hostname", "x"});
  auto r = Execute(ctx, set);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, EngineError::SessionRequired);
  // Nothing crossed the wire.
  std::lock_guard<std::mutex> lk(f.state.mu);
  EXPECT_TRUE(f.state.sets.empty());
}

// gap #8: the engine is the authoritative role gate. Even when a
// command reaches Execute (parsed elsewhere), a caller whose role is
// below the command's gate is rejected before anything crosses the
// wire — role and session are gated on one code path.
TEST(Engine, RoleForbiddenRejectedBeforeWire) {
  Fixture f;
  ASSERT_TRUE(f.tx);
  // `configure` is AdminOnly; parse it as admin, then drive Execute
  // with a downgraded caller to isolate the engine's own role gate.
  auto configure = ParseIn(f.tree, {"configure"});
  auto ctx = f.Ctx();
  ctx.caller.role = RoleGate::AnyAuthenticated;

  auto r = Execute(ctx, configure);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, EngineError::RoleForbidden);
  // The session was never opened and nothing hit the daemon.
  EXPECT_FALSE(f.session.in_configure);
  // The rejection is audited.
  ASSERT_FALSE(f.audit_log.empty());
  EXPECT_EQ(f.audit_log.back().outcome, "role forbidden");
  EXPECT_FALSE(f.audit_log.back().ok);
}

// An operator may run an OperatorOrAdmin command but not an AdminOnly
// one — the gate honours the ladder, not just admin/deny.
TEST(Engine, OperatorRoleGatedByLadder) {
  Fixture f;
  ASSERT_TRUE(f.tx);
  auto configure = ParseIn(f.tree, {"configure"});  // AdminOnly
  auto ctx = f.Ctx();
  ctx.caller.role = RoleGate::OperatorOrAdmin;
  auto r = Execute(ctx, configure);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, EngineError::RoleForbidden);
}

TEST(Engine, RollbackCandidateClearsSession) {
  Fixture f;
  ASSERT_TRUE(f.tx);
  auto ctx = f.Ctx();

  ASSERT_TRUE(Execute(ctx, ParseIn(f.tree, {"configure"})).has_value());
  EXPECT_TRUE(f.session.in_configure);

  auto rb = ParseIn(f.tree, {"rollback", "candidate"});
  ASSERT_TRUE(Execute(ctx, rb).has_value());
  EXPECT_FALSE(f.session.in_configure);
  EXPECT_FALSE(f.session.session_id.has_value());
}

TEST(Engine, LocalVerbRejected) {
  Fixture f;
  auto ctx = f.Ctx();
  // `help` is framework-local (empty wire_command).
  auto help = ParseIn(f.tree, {"help"});
  auto r = Execute(ctx, help);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, EngineError::NotAWireCommand);
}

TEST(Engine, EmitsAuditRecordPerCommand) {
  Fixture f;
  ASSERT_TRUE(f.tx);
  auto ctx = f.Ctx();

  ASSERT_TRUE(Execute(ctx, ParseIn(f.tree, {"configure"})).has_value());
  ASSERT_TRUE(
      Execute(ctx, ParseIn(f.tree, {"set", "hostname", "demo"})).has_value());

  ASSERT_EQ(f.audit_log.size(), 2u);
  const auto &configure = f.audit_log[0];
  EXPECT_EQ(configure.command, "configure");
  EXPECT_EQ(configure.wire_command, "configure");
  EXPECT_EQ(configure.user, "root");
  EXPECT_EQ(configure.role, "admin");
  EXPECT_TRUE(configure.ok);
  EXPECT_EQ(configure.outcome, "ok");
  EXPECT_FALSE(configure.timestamp.empty());

  const auto &set = f.audit_log[1];
  EXPECT_EQ(set.command, "set");
  EXPECT_EQ(set.args.size(), 2u);
  EXPECT_EQ(set.session_id.value_or(""), "sess-7");
  EXPECT_TRUE(set.ok);
}

TEST(Engine, RejectionIsAudited) {
  Fixture f;
  auto ctx = f.Ctx();
  (void)Execute(ctx, ParseIn(f.tree, {"set", "hostname", "x"}));
  ASSERT_EQ(f.audit_log.size(), 1u);
  EXPECT_FALSE(f.audit_log[0].ok);
  EXPECT_EQ(f.audit_log[0].outcome, "session required");
}

TEST(Engine, DaemonErrorSurfacedAndAudited) {
  Fixture f;
  ASSERT_TRUE(f.tx);
  auto ctx = f.Ctx();
  // Open a session, then fake a stale-session set by clearing the
  // daemon's session out from under us via a second commit path is
  // overkill — instead drive `show config` which the fake daemon
  // answers Ok. To force a daemon Error we send `set` with a bogus
  // session id by opening then manually corrupting the client id.
  ASSERT_TRUE(Execute(ctx, ParseIn(f.tree, {"configure"})).has_value());
  f.session.session_id = "wrong";
  auto r = Execute(ctx, ParseIn(f.tree, {"set", "a", "b"}));
  ASSERT_TRUE(r.has_value());
  ASSERT_TRUE(r->response.has_value());
  EXPECT_EQ(r->response->status, protocol::ResponseStatus::Error);
  ASSERT_FALSE(f.audit_log.empty());
  const auto &last = f.audit_log.back();
  EXPECT_FALSE(last.ok);
  EXPECT_EQ(last.outcome, "no_session");
}

TEST(Engine, WireTimeoutClassified) {
  // Point at an endpoint with no daemon bound; a short timeout should
  // resolve as WireStatus::Timeout rather than an exception.
  CommandTree tree;
  (void)RegisterGlobals(tree);
  Session session;
  auto tx = Connect("ipc:///tmp/einheit_engine_nodaemon.ctl",
                    "ipc:///tmp/einheit_engine_nodaemon.pub");
  ASSERT_TRUE(tx);

  Context ctx;
  ctx.tx = tx.get();
  ctx.session = &session;
  ctx.caller.role = RoleGate::AdminOnly;
  ctx.timeout = std::chrono::milliseconds(200);

  auto p = Parse(tree, {"show", "config"}, RoleGate::AdminOnly);
  ASSERT_TRUE(p.has_value());
  auto r = Execute(ctx, *p);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->wire, WireStatus::Timeout);
}

}  // namespace
}  // namespace einheit::cli::engine
