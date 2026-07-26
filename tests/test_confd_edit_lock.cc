/// @file test_confd_edit_lock.cc
/// @brief Edit-locking behaviour: the lock module itself, and the
/// Runtime semantics built on it.
///
/// The Runtime-level tests deliberately use two Runtimes over ONE state
/// directory, because that is the deployment that makes edit locking
/// necessary in the first place: a product that embeds confd in its CLI
/// binary has one Runtime per logged-in operator, so an in-process flag
/// would let two operators hold two candidates and clobber each other.
// Copyright (c) 2026 Einheit Networks

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/confd/edit_lock.h"
#include "einheit/cli/confd/memory_backend.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

// A unique temp state dir per test; removed on destruction.
struct TempDir {
  fs::path path;
  explicit TempDir(const std::string &name)
      : path(fs::temp_directory_path() /
             ("einheit_lock_test_" + name + "_" +
              std::to_string(::getpid()))) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    fs::remove_all(path);
  }
  auto str() const -> std::string {
    return path.string();
  }
};

// A pid that is guaranteed not to exist: fork a child, let it exit,
// reap it. Nothing else can be holding a lock under this pid.
auto DeadPid() -> std::int64_t {
  const pid_t pid = ::fork();
  if (pid == 0) ::_exit(0);
  int status = 0;
  ::waitpid(pid, &status, 0);
  return static_cast<std::int64_t>(pid);
}

auto MakeLock(std::string holder, std::string session, std::int64_t pid)
    -> EditLock {
  EditLock l;
  l.holder = std::move(holder);
  l.session_id = std::move(session);
  l.pid = pid;
  l.acquired_at = "2026-07-26T12:00:00Z";
  return l;
}

auto Req(const std::string &command, std::vector<std::string> args = {},
         std::optional<std::string> session = std::nullopt,
         const std::string &user = "root") -> protocol::Request {
  protocol::Request r;
  r.id = "t";
  r.user = user;
  r.role = "admin";
  r.command = command;
  r.args = std::move(args);
  r.session_id = std::move(session);
  return r;
}

auto Body(const protocol::Response &r) -> std::string {
  return std::string(r.data.begin(), r.data.end());
}

auto Ok(const protocol::Response &r) -> bool {
  return r.status == protocol::ResponseStatus::Ok;
}

constexpr const char *kSchemaYaml = R"yaml(
version: 1
product: lock-test

config:
  name:
    type: string
    help: "Free string"
  port:
    type: integer
    range: [1, 100]
    help: "Small int"

types: {}
)yaml";

auto TestSchema() -> std::shared_ptr<const schema::Schema> {
  auto s = schema::LoadSchemaFromString(kSchemaYaml);
  return s ? *s : std::make_shared<const schema::Schema>();
}

// ── The lock module ─────────────────────────────────────────────

TEST(EditLock, FreeDirectoryHasNoHolder) {
  TempDir dir("free");
  auto held = ReadEditLock(dir.str());
  ASSERT_TRUE(held.has_value());
  EXPECT_FALSE(held->has_value());
}

TEST(EditLock, AcquireIsExclusive) {
  TempDir dir("exclusive");
  auto first = AcquireEditLock(
      dir.str(), MakeLock("alice", "s-1", ::getpid()), /*force=*/false);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(first->stolen_from);
  EXPECT_FALSE(first->reclaimed_from);

  auto second = AcquireEditLock(
      dir.str(), MakeLock("bob", "s-2", ::getpid()), /*force=*/false);
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, LockError::Held);
  // The refusal has to name the holder — an operator who cannot see who
  // is editing has no way forward but to guess.
  EXPECT_NE(second.error().message.find("alice"), std::string::npos);
  EXPECT_NE(second.error().message.find("s-1"), std::string::npos);
  // Short enough to survive the CLI's one-line, truncating error box.
  EXPECT_LT(second.error().message.size(), 78u) << second.error().message;
}

TEST(EditLock, ForceStealsAndReportsPreviousHolder) {
  TempDir dir("force");
  ASSERT_TRUE(AcquireEditLock(dir.str(),
                              MakeLock("alice", "s-1", ::getpid()), false)
                  .has_value());
  auto stolen = AcquireEditLock(
      dir.str(), MakeLock("bob", "s-2", ::getpid()), /*force=*/true);
  ASSERT_TRUE(stolen.has_value());
  ASSERT_TRUE(stolen->stolen_from.has_value());
  EXPECT_EQ(stolen->stolen_from->holder, "alice");
  EXPECT_EQ(stolen->stolen_from->session_id, "s-1");

  auto now = ReadEditLock(dir.str());
  ASSERT_TRUE(now.has_value());
  ASSERT_TRUE(now->has_value());
  EXPECT_EQ((*now)->holder, "bob");
}

TEST(EditLock, StaleLockFromADeadHolderIsReclaimedWithoutForce) {
  TempDir dir("stale");
  const auto dead = DeadPid();
  ASSERT_TRUE(
      AcquireEditLock(dir.str(), MakeLock("ghost", "s-old", dead), false)
          .has_value());
  // No force: "the lock dies with the session" means a holder whose
  // process is gone must not wedge configure mode for everyone else.
  auto taken = AcquireEditLock(
      dir.str(), MakeLock("bob", "s-2", ::getpid()), /*force=*/false);
  ASSERT_TRUE(taken.has_value());
  ASSERT_TRUE(taken->reclaimed_from.has_value());
  EXPECT_EQ(taken->reclaimed_from->holder, "ghost");
  EXPECT_FALSE(taken->stolen_from);
}

TEST(EditLock, UnknownPidCountsAsAlive) {
  // pid 0 means "we cannot probe this"; refusing is the safe answer.
  EXPECT_TRUE(EditLockHolderAlive(MakeLock("who", "s", 0)));
  EXPECT_FALSE(EditLockHolderAlive(MakeLock("ghost", "s", DeadPid())));
  EXPECT_TRUE(EditLockHolderAlive(MakeLock("us", "s", ::getpid())));
}

TEST(EditLock, ReacquiringOwnSessionRefreshes) {
  TempDir dir("refresh");
  ASSERT_TRUE(AcquireEditLock(dir.str(),
                              MakeLock("alice", "s-1", ::getpid()), false)
                  .has_value());
  auto again = AcquireEditLock(
      dir.str(), MakeLock("alice", "s-1", ::getpid()), /*force=*/false);
  ASSERT_TRUE(again.has_value());
  EXPECT_FALSE(again->stolen_from);
  EXPECT_FALSE(again->reclaimed_from);
}

TEST(EditLock, ReleaseOnlyWorksForTheHolder) {
  TempDir dir("release");
  ASSERT_TRUE(AcquireEditLock(dir.str(),
                              MakeLock("alice", "s-1", ::getpid()), false)
                  .has_value());
  // A session that was force-stolen must not delete the thief's lock on
  // its way out.
  ReleaseEditLock(dir.str(), "s-someone-else");
  auto still = ReadEditLock(dir.str());
  ASSERT_TRUE(still.has_value());
  ASSERT_TRUE(still->has_value());
  EXPECT_EQ((*still)->session_id, "s-1");

  ReleaseEditLock(dir.str(), "s-1");
  auto gone = ReadEditLock(dir.str());
  ASSERT_TRUE(gone.has_value());
  EXPECT_FALSE(gone->has_value());
}

TEST(EditLock, CorruptLockFileIsTreatedAsStaleNotAsALockout) {
  TempDir dir("corrupt");
  std::ofstream(dir.path / "configure.lock") << "garbage\n";
  // A lock file is a record of who is editing, not a security
  // boundary: an unparseable one must not brick configure mode.
  auto taken = AcquireEditLock(
      dir.str(), MakeLock("bob", "s-2", ::getpid()), /*force=*/false);
  ASSERT_TRUE(taken.has_value());
  auto now = ReadEditLock(dir.str());
  ASSERT_TRUE(now.has_value());
  ASSERT_TRUE(now->has_value());
  EXPECT_EQ((*now)->holder, "bob");
}

// ── Runtime semantics ───────────────────────────────────────────

TEST(ConfdEditLock, SecondRuntimeIsRefusedAndTheHolderIsNamed) {
  TempDir dir("two_runtimes");
  MemoryBackend backend(TestSchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime a(backend, opts);
  Runtime b(backend, opts);

  auto first = a.HandleRequest(Req("configure", {}, std::nullopt, "alice"));
  ASSERT_TRUE(Ok(first));
  auto second = b.HandleRequest(Req("configure", {}, std::nullopt, "bob"));
  ASSERT_FALSE(Ok(second));
  ASSERT_TRUE(second.error.has_value());
  EXPECT_EQ(second.error->code, "session_busy");
  EXPECT_NE(second.error->message.find("alice"), std::string::npos);
  // The way out belongs in `hint`, not appended to `message`: the CLI
  // renders the message on one line and truncates the overflow, which
  // would cut off exactly the actionable half.
  EXPECT_NE(second.error->hint.find("configure force"),
            std::string::npos);
}

TEST(ConfdEditLock, ConfigureForceTakesOverAcrossRuntimes) {
  TempDir dir("force_across");
  MemoryBackend backend(TestSchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime a(backend, opts);
  Runtime b(backend, opts);

  ASSERT_TRUE(
      Ok(a.HandleRequest(Req("configure", {}, std::nullopt, "alice"))));
  auto stolen =
      b.HandleRequest(Req("configure_force", {}, std::nullopt, "bob"));
  ASSERT_TRUE(Ok(stolen));
  const auto session_b = Body(stolen);
  EXPECT_FALSE(session_b.empty());

  auto held = b.EditLockState();
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(held->holder, "bob");
  EXPECT_EQ(held->session_id, session_b);
}

TEST(ConfdEditLock, AStolenSessionCannotCommit) {
  TempDir dir("stolen_commit");
  MemoryBackend backend(TestSchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime a(backend, opts);
  Runtime b(backend, opts);

  auto opened = a.HandleRequest(Req("configure", {}, std::nullopt, "alice"));
  ASSERT_TRUE(Ok(opened));
  const auto session_a = Body(opened);
  ASSERT_TRUE(Ok(a.HandleRequest(Req("set", {"name", "alice-was-here"},
                                     session_a, "alice"))));

  ASSERT_TRUE(Ok(
      b.HandleRequest(Req("configure_force", {}, std::nullopt, "bob"))));

  // Alice's candidate must not reach the box: she lost the lock, and
  // committing anyway would silently discard whatever bob is doing.
  auto commit =
      a.HandleRequest(Req("commit", {}, session_a, "alice"));
  ASSERT_FALSE(Ok(commit));
  ASSERT_TRUE(commit.error.has_value());
  EXPECT_EQ(commit.error->code, "lock_lost");
  EXPECT_NE(commit.error->hint.find("bob"), std::string::npos);
  EXPECT_NE(commit.error->hint.find("configure force"),
            std::string::npos);
  EXPECT_EQ(backend.ApplyCount(), 0);
  EXPECT_FALSE(backend.DeviceState().contains("name"));
}

TEST(ConfdEditLock, CommitReleasesTheLock) {
  TempDir dir("commit_release");
  MemoryBackend backend(TestSchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime a(backend, opts);
  Runtime b(backend, opts);

  auto opened = a.HandleRequest(Req("configure", {}, std::nullopt, "alice"));
  ASSERT_TRUE(Ok(opened));
  ASSERT_TRUE(Ok(a.HandleRequest(Req("commit", {}, Body(opened), "alice"))));
  EXPECT_FALSE(a.EditLockState().has_value());
  // bob can now edit without forcing.
  EXPECT_TRUE(
      Ok(b.HandleRequest(Req("configure", {}, std::nullopt, "bob"))));
}

TEST(ConfdEditLock, RollbackCandidateReleasesTheLock) {
  TempDir dir("rollback_release");
  MemoryBackend backend(TestSchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime a(backend, opts);
  Runtime b(backend, opts);

  auto opened = a.HandleRequest(Req("configure", {}, std::nullopt, "alice"));
  ASSERT_TRUE(Ok(opened));
  // This is the path `exit` takes out of configure mode, so it is the
  // one that decides whether leaving the shell frees the lock.
  ASSERT_TRUE(Ok(a.HandleRequest(
      Req("rollback", {"candidate"}, Body(opened), "alice"))));
  EXPECT_FALSE(a.EditLockState().has_value());
  EXPECT_TRUE(
      Ok(b.HandleRequest(Req("configure", {}, std::nullopt, "bob"))));
}

TEST(ConfdEditLock, TheLockDiesWithTheRuntime) {
  TempDir dir("runtime_death");
  MemoryBackend backend(TestSchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  {
    Runtime a(backend, opts);
    ASSERT_TRUE(
        Ok(a.HandleRequest(Req("configure", {}, std::nullopt, "alice"))));
  }
  auto held = ReadEditLock(dir.str());
  ASSERT_TRUE(held.has_value());
  EXPECT_FALSE(held->has_value());

  Runtime b(backend, opts);
  EXPECT_TRUE(
      Ok(b.HandleRequest(Req("configure", {}, std::nullopt, "bob"))));
}

TEST(ConfdEditLock, ShowStatusReportsTheHolder) {
  TempDir dir("status");
  MemoryBackend backend(TestSchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime rt(backend, opts);

  auto free_status = Body(rt.HandleRequest(Req("show_status")));
  EXPECT_NE(free_status.find("lock_holder=<none>"), std::string::npos);

  auto opened = rt.HandleRequest(Req("configure", {}, std::nullopt, "alice"));
  ASSERT_TRUE(Ok(opened));
  auto busy_status = Body(rt.HandleRequest(Req("show_status")));
  EXPECT_NE(busy_status.find("lock_holder=alice"), std::string::npos);
  EXPECT_NE(busy_status.find("lock_session=" + Body(opened)),
            std::string::npos);
  EXPECT_NE(busy_status.find("lock_since="), std::string::npos);
}

TEST(ConfdEditLock, InMemoryRuntimeStillRefusesASecondConfigure) {
  // No state dir: the lock degrades to a process-local flag, which is
  // still the whole story for a single-process daemon.
  MemoryBackend backend(TestSchema());
  Runtime rt(backend, {});
  ASSERT_TRUE(
      Ok(rt.HandleRequest(Req("configure", {}, std::nullopt, "alice"))));
  auto second =
      rt.HandleRequest(Req("configure", {}, std::nullopt, "bob"));
  ASSERT_FALSE(Ok(second));
  EXPECT_EQ(second.error->code, "session_busy");
  EXPECT_NE(second.error->message.find("alice"), std::string::npos);
  EXPECT_NE(second.error->hint.find("configure force"),
            std::string::npos);

  auto forced =
      rt.HandleRequest(Req("configure_force", {}, std::nullopt, "bob"));
  EXPECT_TRUE(Ok(forced));
}

TEST(ConfdEditLock, ForcedTakeoverDiscardsThePreviousCandidate) {
  MemoryBackend backend(TestSchema());
  Runtime rt(backend, {});
  auto opened = rt.HandleRequest(Req("configure", {}, std::nullopt, "alice"));
  ASSERT_TRUE(Ok(opened));
  ASSERT_TRUE(Ok(rt.HandleRequest(
      Req("set", {"name", "alice-draft"}, Body(opened), "alice"))));

  auto forced =
      rt.HandleRequest(Req("configure_force", {}, std::nullopt, "bob"));
  ASSERT_TRUE(Ok(forced));
  const auto session_b = Body(forced);
  // bob's candidate is seeded from running, not inherited from alice.
  auto diff = Body(rt.HandleRequest(Req("show_diff", {}, session_b, "bob")));
  EXPECT_NE(diff.find("nothing to commit"), std::string::npos);

  ASSERT_TRUE(Ok(rt.HandleRequest(Req("commit", {}, session_b, "bob"))));
  EXPECT_FALSE(backend.DeviceState().contains("name"));
}

TEST(ConfdEditLock, ASecondRuntimeSeesCommitsMadeByTheFirst) {
  // Two processes over one store: the second must adopt the first's
  // commits when it opens a session, or it seeds a candidate from a
  // stale running config and its commit silently reverts the other
  // operator's work.
  TempDir dir("cross_process_state");
  MemoryBackend backend(TestSchema());
  RuntimeOptions opts;
  opts.state_dir = dir.str();
  Runtime a(backend, opts);
  Runtime b(backend, opts);

  auto opened = a.HandleRequest(Req("configure", {}, std::nullopt, "alice"));
  ASSERT_TRUE(Ok(opened));
  ASSERT_TRUE(Ok(a.HandleRequest(
      Req("set", {"name", "from-alice"}, Body(opened), "alice"))));
  ASSERT_TRUE(Ok(a.HandleRequest(Req("commit", {}, Body(opened), "alice"))));

  auto opened_b = b.HandleRequest(Req("configure", {}, std::nullopt, "bob"));
  ASSERT_TRUE(Ok(opened_b));
  EXPECT_EQ(b.Running().at("name"), "from-alice");
  ASSERT_TRUE(Ok(b.HandleRequest(
      Req("set", {"port", "42"}, Body(opened_b), "bob"))));
  ASSERT_TRUE(Ok(b.HandleRequest(Req("commit", {}, Body(opened_b), "bob"))));
  // bob's commit carries alice's change forward instead of dropping it.
  EXPECT_EQ(backend.DeviceState().at("name"), "from-alice");
  EXPECT_EQ(backend.DeviceState().at("port"), "42");
}

}  // namespace
}  // namespace einheit::cli::confd
