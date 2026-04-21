/// @file test_oneshot.cc
/// @brief RunOneshot tests against a fake daemon.
// Copyright (c) 2026 Einheit Networks

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/shell.h"
#include "einheit/cli/transport/zmq_local.h"

#include "tests/fake_daemon.h"

namespace einheit::cli::shell {
namespace {

class NoopAdapter : public ProductAdapter {
 public:
  auto Metadata() const -> ProductMetadata override {
    ProductMetadata m;
    m.prompt = "t";
    return m;
  }
  auto GetSchema() const -> const schema::Schema & override {
    return schema_;
  }
  auto ControlSocketPath() const -> std::string override { return ""; }
  auto EventSocketPath() const -> std::string override { return ""; }
  auto Commands() const -> std::vector<CommandSpec> override {
    CommandSpec c;
    c.path = "show status";
    c.wire_command = "show_status";
    return {c};
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

auto BuildShell(const einheit::cli::testing::FakeDaemon &d,
                RoleGate role = RoleGate::AdminOnly)
    -> std::pair<Shell, bool> {
  Shell s;
  s.caller.role = role;
  s.caller.user = "test";
  auto adapter = std::make_unique<NoopAdapter>();
  if (auto r = RegisterGlobals(s.tree); !r) return {std::move(s), false};
  for (auto &spec : adapter->Commands()) {
    if (auto r = Register(s.tree, std::move(spec)); !r) {
      return {std::move(s), false};
    }
  }

  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = d.ControlEndpoint();
  cfg.event_endpoint = d.EventEndpoint();
  auto tx = transport::NewZmqLocalTransport(cfg);
  if (!tx) return {std::move(s), false};
  if (auto r = (*tx)->Connect(); !r) return {std::move(s), false};
  s.tx = std::move(*tx);
  s.adapter = std::move(adapter);
  return {std::move(s), true};
}

}  // namespace

TEST(RunOneshot, DispatchesAndReturnsResponse) {
  einheit::cli::testing::FakeDaemon daemon(
      [](const protocol::Request &req) {
        protocol::Response r;
        EXPECT_EQ(req.command, "show_status");
        r.status = protocol::ResponseStatus::Ok;
        return r;
      });

  auto [s, ok] = BuildShell(daemon);
  ASSERT_TRUE(ok);

  auto r = RunOneshot(s, {"show", "status"});
  ASSERT_TRUE(r.has_value()) << r.error().message;
  ASSERT_TRUE(r->response.has_value());
  EXPECT_EQ(r->response->status, protocol::ResponseStatus::Ok);
}

TEST(RunOneshot, RejectsConfigureSessionCommands) {
  einheit::cli::testing::FakeDaemon daemon(
      [](const protocol::Request &) { return protocol::Response{}; });
  auto [s, ok] = BuildShell(daemon);
  ASSERT_TRUE(ok);

  auto r = RunOneshot(s, {"commit"});
  EXPECT_FALSE(r.has_value());
}

TEST(RunOneshot, UnknownCommandSurfacesError) {
  einheit::cli::testing::FakeDaemon daemon(
      [](const protocol::Request &) { return protocol::Response{}; });
  auto [s, ok] = BuildShell(daemon);
  ASSERT_TRUE(ok);

  auto r = RunOneshot(s, {"bogus"});
  EXPECT_FALSE(r.has_value());
}

TEST(RunOneshot, LocalExitHandledInline) {
  einheit::cli::testing::FakeDaemon daemon(
      [](const protocol::Request &) { return protocol::Response{}; });
  auto [s, ok] = BuildShell(daemon);
  ASSERT_TRUE(ok);

  auto r = RunOneshot(s, {"exit"});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->handled_locally);
  EXPECT_TRUE(r->exit_shell);
  EXPECT_FALSE(r->response.has_value());
}

}  // namespace einheit::cli::shell
