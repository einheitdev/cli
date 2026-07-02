/// @file test_confd_runtime.cc
/// @brief Behavioural tests for the confd runtime driven through its
/// transport-agnostic HandleRequest core, against the fake-hardware
/// MemoryBackend. Assertions observe real device state changes — a
/// stub that returned Ok without applying would fail them.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/confd/memory_backend.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {
namespace {

// A permissive schema so `set` validation accepts arbitrary paths in
// these lifecycle tests. An empty schema treats unknown paths as
// free-form strings.
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

// Drive a full configure/set/commit and return the issued session id.
auto Configure(Runtime &rt) -> std::string {
  auto r = rt.HandleRequest(Req("configure"));
  EXPECT_EQ(r.status, protocol::ResponseStatus::Ok);
  return DataString(r);
}

TEST(ConfdRuntime, CommitProgramsTheDevice) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);

  const auto sid = Configure(rt);
  ASSERT_FALSE(sid.empty());

  // Before commit the device is untouched.
  EXPECT_TRUE(backend.DeviceState().empty());

  auto set = rt.HandleRequest(Req("set", {"hostname", "einheit-1"}, sid));
  EXPECT_EQ(set.status, protocol::ResponseStatus::Ok);

  // Still untouched — set only edits the candidate.
  EXPECT_TRUE(backend.DeviceState().empty());

  auto commit = rt.HandleRequest(Req("commit", {}, sid));
  ASSERT_EQ(commit.status, protocol::ResponseStatus::Ok);

  // Now the box actually holds the config.
  auto dev = backend.DeviceState();
  EXPECT_EQ(dev["hostname"], "einheit-1");
  EXPECT_EQ(backend.ApplyCount(), 1);
  EXPECT_EQ(rt.HistorySize(), 1u);
}

TEST(ConfdRuntime, SetRequiresMatchingSession) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  Configure(rt);
  // Wrong session id is rejected.
  auto set = rt.HandleRequest(Req("set", {"a", "b"}, "wrong"));
  EXPECT_EQ(set.status, protocol::ResponseStatus::Error);
  ASSERT_TRUE(set.error.has_value());
  EXPECT_EQ(set.error->code, "no_session");
}

TEST(ConfdRuntime, SecondConfigureIsRefused) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  Configure(rt);
  auto second = rt.HandleRequest(Req("configure"));
  EXPECT_EQ(second.status, protocol::ResponseStatus::Error);
  ASSERT_TRUE(second.error.has_value());
  EXPECT_EQ(second.error->code, "session_busy");
}

TEST(ConfdRuntime, RollbackCandidateDiscardsEdits) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  auto sid = Configure(rt);
  rt.HandleRequest(Req("set", {"hostname", "gone"}, sid));
  auto rb = rt.HandleRequest(Req("rollback", {"candidate"}, sid));
  EXPECT_EQ(rb.status, protocol::ResponseStatus::Ok);
  // Nothing was applied; a fresh configure works (session released).
  EXPECT_TRUE(backend.DeviceState().empty());
  auto sid2 = Configure(rt);
  EXPECT_FALSE(sid2.empty());
}

TEST(ConfdRuntime, RollbackPreviousReappliesPriorCommit) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);

  auto s1 = Configure(rt);
  rt.HandleRequest(Req("set", {"mode", "active"}, s1));
  rt.HandleRequest(Req("commit", {}, s1));
  EXPECT_EQ(backend.DeviceState()["mode"], "active");

  auto s2 = Configure(rt);
  rt.HandleRequest(Req("set", {"mode", "standby"}, s2));
  rt.HandleRequest(Req("commit", {}, s2));
  EXPECT_EQ(backend.DeviceState()["mode"], "standby");

  // rollback previous re-applies commit 1's candidate as a new commit.
  auto rb = rt.HandleRequest(Req("rollback_previous"));
  ASSERT_EQ(rb.status, protocol::ResponseStatus::Ok);
  EXPECT_EQ(backend.DeviceState()["mode"], "active");
  EXPECT_EQ(rt.HistorySize(), 3u);
}

TEST(ConfdRuntime, RollbackToSpecificRevision) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  auto s1 = Configure(rt);
  rt.HandleRequest(Req("set", {"n", "1"}, s1));
  auto c1 = rt.HandleRequest(Req("commit", {}, s1));
  // commit_id=1
  EXPECT_EQ(DataString(c1), "commit_id=1");

  auto s2 = Configure(rt);
  rt.HandleRequest(Req("set", {"n", "2"}, s2));
  rt.HandleRequest(Req("commit", {}, s2));
  EXPECT_EQ(backend.DeviceState()["n"], "2");

  auto rb = rt.HandleRequest(Req("rollback_to", {"1"}));
  ASSERT_EQ(rb.status, protocol::ResponseStatus::Ok);
  EXPECT_EQ(backend.DeviceState()["n"], "1");
}

TEST(ConfdRuntime, FailedApplyDoesNotAdvanceRunning) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);

  auto s1 = Configure(rt);
  rt.HandleRequest(Req("set", {"k", "good"}, s1));
  rt.HandleRequest(Req("commit", {}, s1));
  EXPECT_EQ(backend.DeviceState()["k"], "good");

  auto s2 = Configure(rt);
  rt.HandleRequest(Req("set", {"k", "bad"}, s2));
  backend.FailNextApply();
  auto commit = rt.HandleRequest(Req("commit", {}, s2));
  EXPECT_EQ(commit.status, protocol::ResponseStatus::Error);
  ASSERT_TRUE(commit.error.has_value());
  EXPECT_EQ(commit.error->code, "apply_failed");

  // Running state held at the last good commit; device unchanged.
  EXPECT_EQ(backend.DeviceState()["k"], "good");
  EXPECT_EQ(rt.HistorySize(), 1u);

  // Session survives so the operator can retry; a retry succeeds.
  auto retry = rt.HandleRequest(Req("commit", {}, s2));
  EXPECT_EQ(retry.status, protocol::ResponseStatus::Ok);
  EXPECT_EQ(backend.DeviceState()["k"], "bad");
}

TEST(ConfdRuntime, ShowConfigAndCommitsReflectState) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  auto s1 = Configure(rt);
  rt.HandleRequest(Req("set", {"hostname", "h1"}, s1));
  rt.HandleRequest(Req("commit", {}, s1));

  auto cfg = rt.HandleRequest(Req("show_config"));
  EXPECT_NE(DataString(cfg).find("hostname=h1"), std::string::npos);

  auto commits = rt.HandleRequest(Req("show_commits"));
  EXPECT_NE(DataString(commits).find("commit_id=1"), std::string::npos);
}

// Gap #4: config application must live only in ConfigBackend::Apply,
// reachable only via commit/rollback — never from a read or the render
// surface. Behaviourally: read/show commands never trigger an Apply.
TEST(ConfdRuntime, ReadAndRenderSurfaceNeverApplies) {
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend);
  auto sid = Configure(rt);
  rt.HandleRequest(Req("set", {"hostname", "h"}, sid));
  rt.HandleRequest(Req("commit", {}, sid));
  ASSERT_EQ(backend.ApplyCount(), 1);  // the one commit

  // Every read / status command the render surface is driven from:
  // none of these may reach the box.
  rt.HandleRequest(Req("show_config"));
  rt.HandleRequest(Req("show_config", {"hostname"}));
  rt.HandleRequest(Req("show_commits"));
  rt.HandleRequest(Req("show_commit", {"1"}));
  rt.HandleRequest(Req("show_status"));
  // set edits only the candidate — also no apply.
  auto sid2 = Configure(rt);
  rt.HandleRequest(Req("set", {"hostname", "h2"}, sid2));
  rt.HandleRequest(Req("delete", {"hostname"}, sid2));
  rt.HandleRequest(Req("rollback", {"candidate"}, sid2));

  EXPECT_EQ(backend.ApplyCount(), 1);  // still just the one commit
}

TEST(ConfdRuntime, EmitsAuthoritativeAudit) {
  std::vector<audit::Record> log;
  RuntimeOptions opts;
  opts.audit = [&](const audit::Record &r) { log.push_back(r); };
  MemoryBackend backend(EmptySchema());
  Runtime rt(backend, std::move(opts));

  auto sid = Configure(rt);
  rt.HandleRequest(Req("set", {"hostname", "h"}, sid));
  rt.HandleRequest(Req("commit", {}, sid));

  // configure + set + commit each produced a record.
  ASSERT_GE(log.size(), 3u);
  EXPECT_EQ(log.front().command, "configure");
  EXPECT_EQ(log.back().command, "commit");
  EXPECT_TRUE(log.back().ok);
  EXPECT_EQ(log.back().user, "root");
}

}  // namespace
}  // namespace einheit::cli::confd
