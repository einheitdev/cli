/// @file test_confd_confirm.cc
/// @brief commit-confirmed auto-rollback behaviour — the anti-lockout
/// feature. Proves the revert timer is server-side (fires after the
/// client session is severed), that `confirm` cancels it, that the
/// countdown is queryable from a reconnecting session, that a second
/// commit supersedes the window, and that a pending window survives a
/// confd restart (expired → fires on recovery; live → re-arms).
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/confd/memory_backend.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/confd/store.h"
#include "einheit/cli/confd/zmq_server.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/schema.h"
#include "einheit/cli/transport/zmq_local.h"

namespace einheit::cli::confd {
namespace {

using namespace std::chrono_literals;

auto EmptySchema() -> std::shared_ptr<const schema::Schema> {
  return std::make_shared<const schema::Schema>();
}

auto Req(const std::string &command, std::vector<std::string> args = {},
         std::optional<std::string> session = std::nullopt)
    -> protocol::Request {
  protocol::Request r;
  r.id = "t";
  r.user = "root";
  r.role = "admin";
  r.command = command;
  r.args = std::move(args);
  r.session_id = std::move(session);
  return r;
}

auto DataString(const protocol::Response &r) -> std::string {
  return std::string(r.data.begin(), r.data.end());
}

// A short but non-trivial confirm window (~1.2s) that keeps live-timer
// tests fast. 0.02 minutes = 1.2 seconds.
constexpr const char *kShortWindow = "0.02";

// Poll until pred() or timeout; returns the final pred() value.
template <class F>
auto WaitUntil(F pred, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return pred();
}

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const std::string &name)
      : path(std::filesystem::temp_directory_path() /
             ("einheit_confd_confirm_" + name + "_" +
              std::to_string(::getpid()))) {
    std::filesystem::remove_all(path);
  }
  ~TempDir() {
    std::filesystem::remove_all(path);
  }
  auto str() const -> std::string {
    return path.string();
  }
};

// Plain commit of one key=value, returns nothing. Opens + closes a
// configure session.
auto PlainCommit(Runtime &rt, const std::string &k, const std::string &v)
    -> void {
  const auto sid = DataString(rt.HandleRequest(Req("configure")));
  rt.HandleRequest(Req("set", {k, v}, sid));
  ASSERT_EQ(rt.HandleRequest(Req("commit", {}, sid)).status,
            protocol::ResponseStatus::Ok);
}

// commit confirmed of one key=value with the given window.
auto ConfirmedCommit(Runtime &rt, const std::string &k, const std::string &v,
                     const std::string &window) -> protocol::Response {
  const auto sid = DataString(rt.HandleRequest(Req("configure")));
  rt.HandleRequest(Req("set", {k, v}, sid));
  return rt.HandleRequest(Req("commit_confirmed", {window}, sid));
}

TEST(ConfdConfirm, AutoRevertFiresAfterDeadline) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);

  PlainCommit(rt, "mode", "active");
  auto r = ConfirmedCommit(rt, "mode", "standby", kShortWindow);
  ASSERT_EQ(r.status, protocol::ResponseStatus::Ok);
  EXPECT_EQ(backend.DeviceState().at("mode"), "standby");
  EXPECT_TRUE(rt.PendingConfirmState().armed);

  // Nobody confirms — the server-side timer reverts to the prior commit.
  const bool reverted = WaitUntil(
      [&] { return backend.DeviceState().at("mode") == "active"; }, 5s);
  EXPECT_TRUE(reverted);
  EXPECT_FALSE(rt.PendingConfirmState().armed);
  // The revert is itself a recorded commit (active, standby, revert).
  EXPECT_EQ(rt.HistorySize(), 3u);
}

TEST(ConfdConfirm, FirstConfirmedCommitRevertsToEmpty) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);

  // No baseline commit: the confirmed commit is rev 1, so the revert
  // target is the empty config.
  auto r = ConfirmedCommit(rt, "hostname", "risky", kShortWindow);
  ASSERT_EQ(r.status, protocol::ResponseStatus::Ok);
  EXPECT_EQ(backend.DeviceState().at("hostname"), "risky");

  const bool emptied =
      WaitUntil([&] { return backend.DeviceState().empty(); }, 5s);
  EXPECT_TRUE(emptied);
}

TEST(ConfdConfirm, ConfirmCancelsAutoRevert) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);

  PlainCommit(rt, "mode", "active");
  // A longer window so we confirm well before it could fire.
  ConfirmedCommit(rt, "mode", "standby", "0.1");  // 6s
  ASSERT_TRUE(rt.PendingConfirmState().armed);

  auto c = rt.HandleRequest(Req("confirm"));
  ASSERT_EQ(c.status, protocol::ResponseStatus::Ok);
  EXPECT_FALSE(rt.PendingConfirmState().armed);

  // Give any (incorrectly-live) timer time to misfire; config must
  // stay at the confirmed value.
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(backend.DeviceState().at("mode"), "standby");
  EXPECT_FALSE(rt.PendingConfirmState().armed);

  // Confirming again is a clean error, not a crash.
  EXPECT_EQ(rt.HandleRequest(Req("confirm")).status,
            protocol::ResponseStatus::Error);
}

TEST(ConfdConfirm, CountdownQueryableFromStatus) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  PlainCommit(rt, "mode", "active");
  ConfirmedCommit(rt, "mode", "standby", "1");  // 60s window

  auto status = rt.HandleRequest(Req("show_status"));
  const auto body = DataString(status);
  EXPECT_NE(body.find("confirm_pending=yes"), std::string::npos) << body;
  EXPECT_NE(body.find("confirm_seconds_remaining="), std::string::npos);

  // Clean up so the 60s timer doesn't linger (destructor joins anyway).
  rt.HandleRequest(Req("confirm"));
}

TEST(ConfdConfirm, SecondCommitSupersedesPendingWindow) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  PlainCommit(rt, "mode", "active");
  ConfirmedCommit(rt, "mode", "standby", kShortWindow);
  ASSERT_TRUE(rt.PendingConfirmState().armed);

  // A fresh plain commit before the deadline confirms/supersedes it.
  PlainCommit(rt, "mode", "frozen");
  EXPECT_FALSE(rt.PendingConfirmState().armed);

  // The window must NOT fire — config stays at the superseding commit.
  std::this_thread::sleep_for(1500ms);
  EXPECT_EQ(backend.DeviceState().at("mode"), "frozen");
  EXPECT_FALSE(rt.PendingConfirmState().armed);
}

// The headline constraint: the timer lives in a process that outlives
// the CLI/SSH session. We arm commit-confirmed over a real ZMQ client,
// then DESTROY the client transport (severing the session) and confirm
// the revert still fires from the server.
TEST(ConfdConfirm, RevertFiresAfterClientSevered) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  ZmqServer server(rt);

  auto connect = [&]() -> std::unique_ptr<transport::Transport> {
    transport::ZmqLocalConfig cfg;
    cfg.control_endpoint = server.ControlEndpoint();
    cfg.event_endpoint = server.EventEndpoint();
    auto tx = transport::NewZmqLocalTransport(cfg);
    EXPECT_TRUE(tx.has_value());
    EXPECT_TRUE((*tx)->Connect().has_value());
    return std::move(*tx);
  };

  {
    auto client = connect();
    // Baseline commit.
    auto sid1 = DataString(*client->SendRequest(Req("configure"), 2s));
    client->SendRequest(Req("set", {"mode", "active"}, sid1), 2s);
    client->SendRequest(Req("commit", {}, sid1), 2s);
    // Confirmed commit that would lock us out.
    auto sid2 = DataString(*client->SendRequest(Req("configure"), 2s));
    client->SendRequest(Req("set", {"mode", "standby"}, sid2), 2s);
    auto cc =
        client->SendRequest(Req("commit_confirmed", {kShortWindow}, sid2), 2s);
    ASSERT_TRUE(cc.has_value());
    ASSERT_EQ(cc->status, protocol::ResponseStatus::Ok);
    EXPECT_EQ(backend.DeviceState().at("mode"), "standby");
    // Client (SSH session) dies here — transport destroyed.
  }

  // With no client at all, the server-side timer still reverts.
  const bool reverted = WaitUntil(
      [&] { return backend.DeviceState().at("mode") == "active"; }, 5s);
  EXPECT_TRUE(reverted);

  // A reconnecting session sees the window is gone.
  auto client2 = connect();
  auto status = DataString(*client2->SendRequest(Req("show_status"), 2s));
  EXPECT_NE(status.find("confirm_pending=no"), std::string::npos);
}

TEST(ConfdConfirm, ExpiredWindowFiresOnRestartRecovery) {
  TempDir dir("expired");
  // Build durable state with two commits, then hand-craft an already
  // EXPIRED pending window pointing the revert at commit 1.
  {
    MemoryBackend backend(EmptySchema());
    RuntimeOptions opts;
    opts.state_dir = dir.str();
    Runtime rt(backend, opts);
    PlainCommit(rt, "mode", "active");   // rev 1
    PlainCommit(rt, "mode", "standby");  // rev 2
  }
  auto loaded = LoadState(dir.str());
  ASSERT_TRUE(loaded.has_value());
  loaded->pending.armed = true;
  loaded->pending.pending_commit = 2;
  loaded->pending.rollback_to = 1;
  loaded->pending.deadline_epoch_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() -
      1000;  // already in the past
  ASSERT_TRUE(SaveState(dir.str(), *loaded).has_value());

  // Restart with a fresh (rebooted) backend: recovery must fire the
  // revert immediately in the constructor.
  MemoryBackend rebooted(EmptySchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime rt(rebooted, opts);

  EXPECT_FALSE(rt.PendingConfirmState().armed);
  EXPECT_EQ(rebooted.DeviceState().at("mode"), "active");
  // active, standby, + the recovery revert.
  EXPECT_EQ(rt.HistorySize(), 3u);
}

TEST(ConfdConfirm, LiveWindowReArmsOnRestart) {
  TempDir dir("live");
  {
    MemoryBackend backend(EmptySchema());
    RuntimeOptions opts;
    opts.state_dir = dir.str();
    Runtime rt(backend, opts);
    PlainCommit(rt, "mode", "active");
    PlainCommit(rt, "mode", "standby");
  }
  auto loaded = LoadState(dir.str());
  ASSERT_TRUE(loaded.has_value());
  loaded->pending.armed = true;
  loaded->pending.pending_commit = 2;
  loaded->pending.rollback_to = 1;
  loaded->pending.deadline_epoch_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      60'000;  // still 60s to go
  ASSERT_TRUE(SaveState(dir.str(), *loaded).has_value());

  MemoryBackend rebooted(EmptySchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime rt(rebooted, opts);

  // The window is still live: re-armed, not fired.
  auto pending = rt.PendingConfirmState();
  EXPECT_TRUE(pending.armed);
  EXPECT_EQ(pending.pending_commit, 2u);
  // No revert happened yet.
  EXPECT_EQ(rt.HistorySize(), 2u);

  rt.HandleRequest(Req("confirm"));  // stand it down for a clean exit
}

}  // namespace
}  // namespace einheit::cli::confd
