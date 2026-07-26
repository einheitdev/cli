/// @file test_confd_boot_apply.cc
/// @brief Boot-restore behaviour — a box that survives a reboot.
///
/// The structural hole this closes: without a boot apply, nothing
/// re-programs the committed configuration when the box comes back, so
/// the runtime adopts whatever state the hardware powered on with and
/// the operator's intent is stranded in history. These tests express
/// that as "the box forgot, the durable store did not" and then assert
/// the box ends up holding intent.
// Copyright (c) 2026 Einheit Networks

#include <unistd.h>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/confd/config_file.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string &name)
      : path(fs::temp_directory_path() /
             ("einheit_boot_test_" + name + "_" +
              std::to_string(::getpid()))) {
    fs::remove_all(path);
  }
  ~TempDir() {
    fs::remove_all(path);
  }
  auto str() const -> std::string {
    return path.string();
  }
};

/// A box we can power-cycle. Unlike MemoryBackend it can also report
/// paths that no commit ever set (the DSA port list, a DHCP-acquired
/// address), which is what makes the reconcile direction testable.
class FakeBox : public ConfigBackend {
 public:
  explicit FakeBox(std::shared_ptr<const schema::Schema> schema)
      : schema_(std::move(schema)) {}

  auto Apply(const Candidate &candidate)
      -> std::expected<CommitId, Error<ApplyError>> override {
    std::lock_guard<std::mutex> lk(mu_);
    ++applies_;
    if (fail_next_) {
      fail_next_ = false;
      return std::unexpected(Error<ApplyError>{
          ApplyError::HardwareRejected, "simulated hardware rejection"});
    }
    programmed_ = candidate.values;
    return ++rev_;
  }

  auto ReadRunning() -> Config override {
    std::lock_guard<std::mutex> lk(mu_);
    Config out = programmed_;
    // Box-only state, always reported, never settable.
    for (const auto &[k, v] : reported_) out[k] = v;
    return out;
  }

  auto Schema() const -> const schema::Schema & override {
    return schema_.Get();
  }

  /// What the hardware currently holds.
  auto Programmed() const -> Config {
    std::lock_guard<std::mutex> lk(mu_);
    return programmed_;
  }

  auto Applies() const -> int {
    std::lock_guard<std::mutex> lk(mu_);
    return applies_;
  }

  /// Power-cycle: the box comes up holding `defaults` and nothing else.
  auto PowerOnWith(Config defaults) -> void {
    std::lock_guard<std::mutex> lk(mu_);
    programmed_ = std::move(defaults);
  }

  /// A path the box reports but no commit sets.
  auto Report(const std::string &path, const std::string &value) -> void {
    std::lock_guard<std::mutex> lk(mu_);
    reported_[path] = value;
  }

  auto FailNextApply() -> void {
    std::lock_guard<std::mutex> lk(mu_);
    fail_next_ = true;
  }

 private:
  schema::SchemaHandle schema_;
  mutable std::mutex mu_;
  Config programmed_;
  Config reported_;
  CommitId rev_ = 0;
  int applies_ = 0;
  bool fail_next_ = false;
};

constexpr const char *kSchemaYaml = R"yaml(
version: 1
product: boot-test

config:
  name:
    type: string
    help: "Free string"
  port:
    type: integer
    range: [1, 100]
    help: "Small int"
  link:
    type: string
    help: "Box-reported state"

types: {}
)yaml";

auto TestSchema() -> std::shared_ptr<const schema::Schema> {
  auto s = schema::LoadSchemaFromString(kSchemaYaml);
  return s ? *s : std::make_shared<const schema::Schema>();
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

auto Body(const protocol::Response &r) -> std::string {
  return std::string(r.data.begin(), r.data.end());
}

auto Ok(const protocol::Response &r) -> bool {
  return r.status == protocol::ResponseStatus::Ok;
}

/// One box plus a state dir, restartable.
struct Fixture {
  TempDir dir;
  FakeBox box{TestSchema()};
  std::optional<Runtime> rt;

  explicit Fixture(const std::string &name) : dir(name) {}

  auto Start(const std::string &factory = {}) -> void {
    RuntimeOptions opts;
    opts.state_dir = dir.str();
    opts.factory_config = factory;
    rt.emplace(box, opts);
  }

  /// Commit `values` as one candidate. Returns the commit id text.
  auto Commit(const std::vector<std::pair<std::string, std::string>> &values)
      -> std::string {
    auto opened = rt->HandleRequest(Req("configure"));
    EXPECT_TRUE(Ok(opened));
    const auto session = Body(opened);
    for (const auto &[k, v] : values) {
      EXPECT_TRUE(Ok(rt->HandleRequest(Req("set", {k, v}, session))));
    }
    auto done = rt->HandleRequest(Req("commit", {}, session));
    EXPECT_TRUE(Ok(done));
    return Body(done);
  }

  /// Reboot: the process goes away, the hardware comes up holding
  /// `defaults`, and a fresh Runtime loads the durable store.
  auto Reboot(Config defaults = {}) -> void {
    rt.reset();
    box.PowerOnWith(std::move(defaults));
    Start();
  }

  /// Reboot on a box that ships a factory-defaults file.
  auto Reboot(const std::string &factory) -> void {
    rt.reset();
    box.PowerOnWith({});
    Start(factory);
  }
};

TEST(BootApply, RestoresTheCommittedConfigurationOntoABlankBox) {
  Fixture f("blank_box");
  f.Start();
  f.Commit({{"name", "sw-1"}, {"port", "42"}});
  ASSERT_EQ(f.box.Programmed().at("name"), "sw-1");

  f.Reboot();
  // This is the hole: before the apply the box is blank even though the
  // store still remembers the commit.
  EXPECT_TRUE(f.box.Programmed().empty());

  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->applied);
  EXPECT_EQ(result->paths, 2u);
  EXPECT_FALSE(result->reverted_pending);
  EXPECT_EQ(f.box.Programmed().at("name"), "sw-1");
  EXPECT_EQ(f.box.Programmed().at("port"), "42");
}

TEST(BootApply, IntentBeatsThePowerOnStateOfTheBox) {
  Fixture f("intent_wins");
  f.Start();
  f.Commit({{"name", "committed"}, {"port", "42"}});

  // The box comes back up with a factory hostname — a different value
  // for a path the operator committed. Constructor reconciliation lets
  // reality win per key, which is right for a running daemon and wrong
  // here; the boot apply has to push intent back over it.
  f.Reboot({{"name", "factory-default"}});
  EXPECT_EQ(f.rt->Running().at("name"), "factory-default");

  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(f.box.Programmed().at("name"), "committed");
  EXPECT_EQ(f.rt->Running().at("name"), "committed");
}

TEST(BootApply, BoxOnlyPathsSurviveTheReconcile) {
  Fixture f("box_only");
  f.Start();
  f.box.Report("link", "up");
  f.Commit({{"name", "sw-1"}});

  f.Reboot();
  f.box.Report("link", "down");
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());
  // Intent wins for the committed path, the box fills in the rest.
  EXPECT_EQ(f.rt->Running().at("name"), "sw-1");
  EXPECT_EQ(f.rt->Running().at("link"), "down");
}

TEST(BootApply, WithNoHistoryAndNoFactoryFileNothingIsApplied) {
  Fixture f("no_history");
  f.Start();
  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->applied);
  EXPECT_FALSE(result->seeded_factory);
  EXPECT_EQ(result->commit, 0u);
  EXPECT_EQ(f.box.Applies(), 0);
}

TEST(BootApply, AFactoryFreshBoxIsSeededFromTheShippedDefaults) {
  // "No configuration" is not a neutral state for a switch: every port
  // comes up administratively down, so an unseeded box forwards
  // nothing. The first boot has to land the shipped defaults.
  Fixture f("factory_seed");
  const auto factory = (f.dir.path / "factory.conf").string();
  ASSERT_TRUE(WriteConfigFile(factory, {{"name", "einheit-s5"},
                                        {"port", "1"}})
                  .has_value());
  f.Start(factory);
  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_TRUE(result.has_value())
      << (result ? "" : result.error().message);
  EXPECT_TRUE(result->applied);
  EXPECT_TRUE(result->seeded_factory);
  EXPECT_EQ(result->paths, 2u);
  EXPECT_EQ(f.box.Programmed().at("name"), "einheit-s5");
  // Recorded as a real commit, so `show config`, `show commits` and
  // rollback all have a floor to stand on.
  EXPECT_EQ(f.rt->HistorySize(), 1u);
  EXPECT_EQ(f.rt->Running().at("name"), "einheit-s5");
}

TEST(BootApply, FactorySeedingHappensOnceNotOnEveryBoot) {
  Fixture f("factory_once");
  const auto factory = (f.dir.path / "factory.conf").string();
  ASSERT_TRUE(
      WriteConfigFile(factory, {{"name", "einheit-s5"}}).has_value());
  f.Start(factory);
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());

  // The operator configures the box; the next boot must restore THAT,
  // not quietly reset to defaults.
  f.Commit({{"name", "operator-named"}});
  f.Reboot(factory);
  auto second = f.rt->ApplyRunningAtBoot();
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(second->seeded_factory);
  EXPECT_EQ(f.box.Programmed().at("name"), "operator-named");
}

TEST(BootApply, AnUnparseableFactoryFileFailsLoudly) {
  Fixture f("factory_broken");
  const auto factory = (f.dir.path / "factory.conf").string();
  fs::create_directories(f.dir.path);
  std::ofstream(factory) << "# einheit-config v1\nno-value-here\n";
  f.Start(factory);
  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ApplyError::ValidationFailed);
  EXPECT_NE(result.error().message.find("factory config"),
            std::string::npos);
  EXPECT_EQ(f.rt->HistorySize(), 0u);
}

TEST(BootApply, AFactoryFileThatViolatesTheSchemaIsRejected) {
  Fixture f("factory_invalid");
  const auto factory = (f.dir.path / "factory.conf").string();
  ASSERT_TRUE(WriteConfigFile(factory, {{"port", "999"}}).has_value());
  f.Start(factory);
  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ApplyError::ValidationFailed);
  EXPECT_EQ(f.box.Applies(), 0);
}

TEST(BootApply, RestoresTheLatestCommitAfterARollback) {
  Fixture f("after_rollback");
  f.Start();
  f.Commit({{"name", "first"}});
  f.Commit({{"name", "second"}});
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("rollback_previous"))));
  ASSERT_EQ(f.box.Programmed().at("name"), "first");

  // A rollback is itself a commit, so boot-restore must land on the
  // rolled-back configuration, not on the newest one ever committed.
  f.Reboot();
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());
  EXPECT_EQ(f.box.Programmed().at("name"), "first");
}

TEST(BootApply, AnExpiredConfirmWindowRevertsInsteadOfReapplying) {
  Fixture f("expired_confirm");
  f.Start();
  f.Commit({{"name", "safe"}});

  // A commit-confirmed with a window short enough to be expired by the
  // time the next runtime starts.
  auto opened = f.rt->HandleRequest(Req("configure"));
  ASSERT_TRUE(Ok(opened));
  const auto session = Body(opened);
  ASSERT_TRUE(Ok(f.rt->HandleRequest(
      Req("set", {"name", "risky"}, session))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(
      Req("commit_confirmed", {"0.0005"}, session))));  // 30 ms
  ASSERT_EQ(f.box.Programmed().at("name"), "risky");

  // The box reboots before anyone confirms. The constructor fires the
  // expired revert; the boot apply must then restore the reverted
  // configuration, never the unconfirmed one.
  f.Reboot();
  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(f.box.Programmed().at("name"), "safe");
  EXPECT_EQ(f.rt->Running().at("name"), "safe");
  EXPECT_FALSE(f.rt->PendingConfirmState().armed);
}

TEST(BootApply, AStillArmedConfirmWindowIsRevertedAtBoot) {
  Fixture f("armed_confirm");
  f.Start();
  f.Commit({{"name", "safe"}});
  auto opened = f.rt->HandleRequest(Req("configure"));
  ASSERT_TRUE(Ok(opened));
  const auto session = Body(opened);
  ASSERT_TRUE(Ok(f.rt->HandleRequest(
      Req("set", {"name", "risky"}, session))));
  // A long window: still live when the box comes back.
  ASSERT_TRUE(Ok(f.rt->HandleRequest(
      Req("commit_confirmed", {"60"}, session))));

  f.Reboot();
  ASSERT_TRUE(f.rt->PendingConfirmState().armed);
  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->reverted_pending);
  // Boot is the deadline: the oneshot has no live timer to fire the
  // auto-revert later, so carrying the window forward would quietly
  // make an unconfirmed configuration permanent.
  EXPECT_EQ(f.box.Programmed().at("name"), "safe");
  EXPECT_FALSE(f.rt->PendingConfirmState().armed);
}

TEST(BootApply, AFailedApplyIsReportedAndLeavesRunningAlone) {
  Fixture f("apply_fails");
  f.Start();
  f.Commit({{"name", "sw-1"}});
  f.Reboot();
  f.box.FailNextApply();

  auto result = f.rt->ApplyRunningAtBoot();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ApplyError::HardwareRejected);
  // History is intact, so init can log the failure and a later retry
  // (or an operator) still has the intent to restore.
  EXPECT_EQ(f.rt->HistorySize(), 1u);
  EXPECT_EQ(f.rt->Running().at("name"), "sw-1");
}

TEST(BootApply, RepeatedBootsAreIdempotent) {
  // The bench exit gate is 50 power pulls; the unit-level version of
  // that is: booting again must not grow history or change the box.
  Fixture f("repeated");
  f.Start();
  f.Commit({{"name", "sw-1"}, {"port", "7"}});
  const auto history_after_commit = f.rt->HistorySize();

  for (int boot = 0; boot < 10; ++boot) {
    f.Reboot();
    auto result = f.rt->ApplyRunningAtBoot();
    ASSERT_TRUE(result.has_value()) << "boot " << boot;
    EXPECT_TRUE(result->applied) << "boot " << boot;
    EXPECT_EQ(f.box.Programmed().at("name"), "sw-1") << "boot " << boot;
    EXPECT_EQ(f.box.Programmed().at("port"), "7") << "boot " << boot;
    EXPECT_EQ(f.rt->HistorySize(), history_after_commit) << "boot " << boot;
  }
}

TEST(BootApply, HistoryStaysUsableAfterABootRestore) {
  Fixture f("history_after_boot");
  f.Start();
  f.Commit({{"name", "first"}});
  f.Commit({{"name", "second"}});
  f.Reboot();
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());

  // Boot-restore must not consume history: rollback still works after
  // a reboot, which is the whole reason the store is durable.
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("rollback_previous"))));
  EXPECT_EQ(f.box.Programmed().at("name"), "first");
}

TEST(BootApply, DoesNotDisturbAnOpenSessionlessRuntime) {
  // The boot apply runs before any front-end is reachable, so it must
  // not need or create a configure session — and must leave the edit
  // lock free for the first operator who logs in.
  Fixture f("no_session");
  f.Start();
  f.Commit({{"name", "sw-1"}});
  f.Reboot();
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());
  EXPECT_FALSE(f.rt->EditLockState().has_value());
  EXPECT_TRUE(Ok(f.rt->HandleRequest(Req("configure"))));
}

}  // namespace
}  // namespace einheit::cli::confd
