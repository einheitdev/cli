/// @file test_session_lifecycle.cc
/// @brief Drives Dispatch() through the configure/set/commit arc
/// against a stateful fake daemon.
// Copyright (c) 2026 Einheit Networks

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/session.h"
#include "einheit/cli/shell.h"
#include "einheit/cli/transport/zmq_local.h"

#include "tests/fake_daemon.h"

namespace einheit::cli::shell {
namespace {

using einheit::cli::testing::FakeDaemon;

class NoopAdapter : public ProductAdapter {
 public:
  auto Metadata() const -> ProductMetadata override {
    ProductMetadata m;
    m.id = "test";
    m.prompt = "test";
    return m;
  }
  auto GetSchema() const -> const schema::Schema & override {
    return schema_;
  }
  auto ControlSocketPath() const -> std::string override { return ""; }
  auto EventSocketPath() const -> std::string override { return ""; }
  auto Commands() const -> std::vector<CommandSpec> override {
    return {};
  }
  auto RenderResponse(const CommandSpec &, const protocol::Response &,
                      render::Renderer &) const -> void override {}
  auto EventTopicsFor(const CommandSpec &) const
      -> std::vector<std::string> override {
    return {};
  }
  auto RenderEvent(const std::string &, const protocol::Event &,
                   render::Renderer &) const -> void override {}

 private:
  schema::Schema schema_;
};

// Minimal daemon model that implements just enough of the lifecycle
// to exercise Dispatch.
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
      state.active_session = "sess-42";
      r.status = protocol::ResponseStatus::Ok;
      r.data.assign(state.active_session->begin(),
                    state.active_session->end());
    } else if (req.command == "set") {
      if (!state.active_session ||
          !req.session_id ||
          *req.session_id != *state.active_session) {
        r.status = protocol::ResponseStatus::Error;
        r.error = protocol::ResponseError{
            "no_session", "set without configure", ""};
      } else {
        if (req.args.size() >= 2) {
          state.sets.emplace_back(req.args[0], req.args[1]);
        }
        r.status = protocol::ResponseStatus::Ok;
      }
    } else if (req.command == "commit") {
      if (!state.active_session) {
        r.status = protocol::ResponseStatus::Error;
        r.error = protocol::ResponseError{
            "no_session", "commit without configure", ""};
      } else {
        ++state.commits;
        state.active_session.reset();
        r.status = protocol::ResponseStatus::Ok;
      }
    } else {
      r.status = protocol::ResponseStatus::Ok;
    }
    return r;
  });
}

auto BuildShell(const FakeDaemon &d)
    -> std::pair<Shell, bool> {
  Shell s;
  s.caller.role = RoleGate::AdminOnly;
  s.caller.user = "test";
  if (auto r = RegisterGlobals(s.tree); !r) return {std::move(s), false};

  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = d.ControlEndpoint();
  cfg.event_endpoint = d.EventEndpoint();
  auto tx = transport::NewZmqLocalTransport(cfg);
  if (!tx) return {std::move(s), false};
  if (auto r = (*tx)->Connect(); !r) return {std::move(s), false};
  s.tx = std::move(*tx);
  s.adapter = std::make_unique<NoopAdapter>();
  return {std::move(s), true};
}

auto Exec(Shell &s, const std::vector<std::string> &tokens)
    -> std::expected<DispatchResult, Error<ShellError>> {
  auto parsed = Parse(s.tree, tokens, s.caller.role);
  EXPECT_TRUE(parsed.has_value())
      << (parsed ? "" : parsed.error().message);
  return Dispatch(s, *parsed);
}

}  // namespace

TEST(SessionLifecycle, ConfigureThenCommit) {
  DaemonState st;
  auto daemon = MakeDaemon(st);
  auto [s, ok] = BuildShell(daemon);
  ASSERT_TRUE(ok);

  EXPECT_FALSE(s.session.in_configure);
  ASSERT_TRUE(Exec(s, {"configure"}).has_value());
  EXPECT_TRUE(s.session.in_configure);
  EXPECT_EQ(s.session.session_id.value_or(""), "sess-42");

  // Set requires the session id to be carried.
  auto set_res = Exec(s, {"set", "hostname", "einheit-42"});
  ASSERT_TRUE(set_res.has_value());
  ASSERT_TRUE(set_res->response.has_value());
  EXPECT_EQ(set_res->response->status,
            protocol::ResponseStatus::Ok);
  {
    std::lock_guard<std::mutex> lk(st.mu);
    ASSERT_EQ(st.sets.size(), 1u);
    EXPECT_EQ(st.sets[0].first, "hostname");
    EXPECT_EQ(st.sets[0].second, "einheit-42");
  }

  ASSERT_TRUE(Exec(s, {"commit"}).has_value());
  EXPECT_FALSE(s.session.in_configure);
  EXPECT_FALSE(s.session.session_id.has_value());
  {
    std::lock_guard<std::mutex> lk(st.mu);
    EXPECT_EQ(st.commits, 1);
  }
}

TEST(SessionLifecycle, SetWithoutConfigureRejectedLocally) {
  DaemonState st;
  auto daemon = MakeDaemon(st);
  auto [s, ok] = BuildShell(daemon);
  ASSERT_TRUE(ok);

  auto r = Exec(s, {"set", "hostname", "x"});
  // Dispatch itself rejects the command — no wire round trip.
  EXPECT_FALSE(r.has_value());
  {
    std::lock_guard<std::mutex> lk(st.mu);
    EXPECT_TRUE(st.sets.empty());
  }
}

TEST(SessionLifecycle, RollbackCandidateClearsSession) {
  DaemonState st;
  auto daemon = MakeDaemon(st);
  auto [s, ok] = BuildShell(daemon);
  ASSERT_TRUE(ok);

  ASSERT_TRUE(Exec(s, {"configure"}).has_value());
  EXPECT_TRUE(s.session.in_configure);

  ASSERT_TRUE(Exec(s, {"rollback", "candidate"}).has_value());
  EXPECT_FALSE(s.session.in_configure);
  EXPECT_FALSE(s.session.session_id.has_value());
}

TEST(SessionLifecycle, ExitIsLocal) {
  DaemonState st;
  auto daemon = MakeDaemon(st);
  auto [s, ok] = BuildShell(daemon);
  ASSERT_TRUE(ok);

  auto r = Exec(s, {"exit"});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->handled_locally);
  EXPECT_TRUE(r->exit_shell);
  EXPECT_FALSE(r->response.has_value());
}

}  // namespace einheit::cli::shell
