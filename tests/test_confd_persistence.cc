/// @file test_confd_persistence.cc
/// @brief Durable-persistence behaviour: running config + commit
/// history survive a confd restart AND a simulated reboot (a fresh,
/// blank backend), and `rollback previous` works after restart because
/// history is durable.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/confd/memory_backend.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/confd/store.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {
namespace {

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

// A unique temp state dir per test; removed on destruction.
struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const std::string &name)
      : path(
            std::filesystem::temp_directory_path() /
            ("einheit_confd_test_" + name + "_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(path);
  }
  ~TempDir() {
    std::filesystem::remove_all(path);
  }
  auto str() const -> std::string {
    return path.string();
  }
};

auto Commit(Runtime &rt, const std::string &key, const std::string &value)
    -> void {
  auto cfg = rt.HandleRequest(Req("configure"));
  ASSERT_EQ(cfg.status, protocol::ResponseStatus::Ok);
  const auto sid = DataString(cfg);
  rt.HandleRequest(Req("set", {key, value}, sid));
  auto c = rt.HandleRequest(Req("commit", {}, sid));
  ASSERT_EQ(c.status, protocol::ResponseStatus::Ok);
}

TEST(ConfdPersistence, RunningSurvivesRestart) {
  TempDir dir("restart");
  {
    MemoryBackend backend(EmptySchema());
    RuntimeOptions opts;
    opts.state_dir = dir.str();
    Runtime rt(backend, opts);
    Commit(rt, "hostname", "persisted");
    EXPECT_EQ(rt.HistorySize(), 1u);
  }

  // A state file now exists on disk.
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(dir.str()) /
                                      "confd.state"));

  // Restart: new runtime, same backend contents, same state dir.
  MemoryBackend backend2(EmptySchema());
  RuntimeOptions opts2;
  opts2.state_dir = dir.str();
  Runtime rt2(backend2, opts2);
  EXPECT_EQ(rt2.HistorySize(), 1u);
  EXPECT_EQ(rt2.Running().at("hostname"), "persisted");
}

TEST(ConfdPersistence, HistorySurvivesSimulatedReboot) {
  TempDir dir("reboot");
  {
    MemoryBackend backend(EmptySchema());
    RuntimeOptions opts;
    opts.state_dir = dir.str();
    Runtime rt(backend, opts);
    Commit(rt, "mode", "active");
    Commit(rt, "mode", "standby");
    EXPECT_EQ(rt.HistorySize(), 2u);
  }

  // Simulated reboot: a BRAND-NEW blank backend (the box lost its
  // runtime state) + a fresh runtime over the same durable state dir.
  MemoryBackend rebooted(EmptySchema());
  EXPECT_TRUE(rebooted.DeviceState().empty());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime rt(rebooted, opts);

  // History is durable across the reboot.
  ASSERT_EQ(rt.HistorySize(), 2u);
  EXPECT_EQ(rt.Running().at("mode"), "standby");

  // rollback previous re-applies commit 1's candidate onto the fresh
  // backend — proving the durable history is usable, not just present.
  auto rb = rt.HandleRequest(Req("rollback_previous"));
  ASSERT_EQ(rb.status, protocol::ResponseStatus::Ok)
      << (rb.error ? rb.error->message : "");
  EXPECT_EQ(rebooted.DeviceState().at("mode"), "active");
  EXPECT_EQ(rt.HistorySize(), 3u);
}

TEST(ConfdPersistence, RevisionIdsDoNotCollideAfterRestart) {
  TempDir dir("revids");
  {
    MemoryBackend backend(EmptySchema());
    RuntimeOptions opts;
    opts.state_dir = dir.str();
    Runtime rt(backend, opts);
    Commit(rt, "n", "1");  // rev 1
    Commit(rt, "n", "2");  // rev 2
  }

  // Restart with a fresh backend (its internal counter resets to 0).
  MemoryBackend backend2(EmptySchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime rt(backend2, opts);
  Commit(rt, "n", "3");  // must be rev 3, not a collision with 1

  auto commits = rt.HandleRequest(Req("show_commits"));
  const auto body = DataString(commits);
  EXPECT_NE(body.find("commit_id=1"), std::string::npos);
  EXPECT_NE(body.find("commit_id=2"), std::string::npos);
  EXPECT_NE(body.find("commit_id=3"), std::string::npos);
  EXPECT_EQ(rt.HistorySize(), 3u);
}

TEST(ConfdPersistence, LoadRoundTripsStoreFormat) {
  TempDir dir("roundtrip");
  PersistentState st;
  st.running = {{"a", "1"}, {"b", "two words"}};
  st.next_rev = 5;
  CommitRecord c;
  c.id = 5;
  c.backend_id = 9;
  c.author = "alice";
  c.timestamp = "2026-07-02T00:00:00.000Z";
  c.candidate.values = {{"a", "1"}, {"b", "two words"}};
  st.history.push_back(c);
  st.pending.armed = true;
  st.pending.rollback_to = 4;
  st.pending.deadline_epoch_ms = 1720000000;
  st.pending.pending_commit = 5;

  ASSERT_TRUE(SaveState(dir.str(), st).has_value());
  auto loaded = LoadState(dir.str());
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->next_rev, 5u);
  EXPECT_EQ(loaded->running.at("b"), "two words");
  ASSERT_EQ(loaded->history.size(), 1u);
  EXPECT_EQ(loaded->history[0].author, "alice");
  EXPECT_EQ(loaded->history[0].candidate.values.at("b"), "two words");
  EXPECT_TRUE(loaded->pending.armed);
  EXPECT_EQ(loaded->pending.rollback_to, 4u);
  EXPECT_EQ(loaded->pending.deadline_epoch_ms, 1720000000);
}

TEST(ConfdPersistence, MissingDirIsFirstBootNotError) {
  auto loaded = LoadState("/nonexistent/einheit/confd/dir");
  ASSERT_TRUE(loaded.has_value());
  EXPECT_TRUE(loaded->history.empty());
  EXPECT_EQ(loaded->next_rev, 0u);
}

}  // namespace
}  // namespace einheit::cli::confd
