/// @file test_shell_escape.cc
/// @brief Shell-escape audit bracketing tests against a fake daemon.
// Copyright (c) 2026 Einheit Networks

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/auth.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/shell_escape.h"
#include "einheit/cli/transport/zmq_local.h"

#include "tests/fake_daemon.h"

namespace einheit::cli::shell_escape {
namespace {

using einheit::cli::testing::FakeDaemon;

struct Recorder {
  std::mutex mu;
  std::vector<std::string> commands;
};

auto MakeDaemon(Recorder &r) -> FakeDaemon {
  return FakeDaemon([&](const protocol::Request &req) {
    std::lock_guard<std::mutex> lk(r.mu);
    r.commands.push_back(req.command);
    return protocol::Response{};
  });
}

auto BuildTransport(const FakeDaemon &d)
    -> std::unique_ptr<transport::Transport> {
  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = d.ControlEndpoint();
  cfg.event_endpoint = d.EventEndpoint();
  auto tx = transport::NewZmqLocalTransport(cfg);
  if (!tx) return nullptr;
  if (auto r = (*tx)->Connect(); !r) return nullptr;
  return std::move(*tx);
}

}  // namespace

TEST(ShellEscape, NotifiesDaemonOnEntryAndExit) {
  Recorder rec;
  auto daemon = MakeDaemon(rec);
  auto tx = BuildTransport(daemon);
  ASSERT_NE(tx, nullptr);

  auth::CallerIdentity caller;
  caller.user = "karl";
  caller.role = RoleGate::AdminOnly;

  Hooks hooks;
  hooks.run_shell =
      [](const std::string & /*bash*/) -> int { return 0; };
  auto rc = Escape(*tx, caller, /*locked=*/false, hooks);
  ASSERT_TRUE(rc.has_value()) << rc.error().message;
  EXPECT_EQ(*rc, 0);

  std::lock_guard<std::mutex> lk(rec.mu);
  ASSERT_EQ(rec.commands.size(), 2u);
  EXPECT_EQ(rec.commands[0], "shell_enter");
  EXPECT_EQ(rec.commands[1], "shell_exit");
}

TEST(ShellEscape, ReturnsSubprocessExitCode) {
  Recorder rec;
  auto daemon = MakeDaemon(rec);
  auto tx = BuildTransport(daemon);
  ASSERT_NE(tx, nullptr);

  auth::CallerIdentity caller;
  caller.user = "karl";
  caller.role = RoleGate::AdminOnly;

  Hooks hooks;
  hooks.run_shell =
      [](const std::string & /*bash*/) -> int { return 42; };
  auto rc = Escape(*tx, caller, /*locked=*/false, hooks);
  ASSERT_TRUE(rc.has_value());
  EXPECT_EQ(*rc, 42);
}

TEST(ShellEscape, LockedRefusesEvenForAdmin) {
  Recorder rec;
  auto daemon = MakeDaemon(rec);
  auto tx = BuildTransport(daemon);
  ASSERT_NE(tx, nullptr);

  auth::CallerIdentity caller;
  caller.user = "karl";
  caller.role = RoleGate::AdminOnly;

  Hooks hooks;
  hooks.run_shell = [](const std::string &) -> int {
    ADD_FAILURE() << "subprocess hook should not run when locked";
    return 0;
  };
  auto rc = Escape(*tx, caller, /*locked=*/true, hooks);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code, EscapeError::NotAuthorised);

  // Locked refusal is silent on the wire — no shell_enter or
  // shell_exit, since no shell was spawned.
  std::lock_guard<std::mutex> lk(rec.mu);
  EXPECT_TRUE(rec.commands.empty());
}

TEST(ShellEscape, NonAdminRejected) {
  Recorder rec;
  auto daemon = MakeDaemon(rec);
  auto tx = BuildTransport(daemon);
  ASSERT_NE(tx, nullptr);

  auth::CallerIdentity caller;
  caller.user = "alice";
  caller.role = RoleGate::OperatorOrAdmin;

  auto rc = Escape(*tx, caller);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code, EscapeError::NotAuthorised);
  {
    std::lock_guard<std::mutex> lk(rec.mu);
    EXPECT_TRUE(rec.commands.empty());
  }
}

}  // namespace einheit::cli::shell_escape
