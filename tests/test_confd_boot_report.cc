/// @file test_confd_boot_report.cc
/// @brief The boot report: persistence, the this-boot test, and the
/// `show system boot` surface.
///
/// The case worth the most attention here is the one where the boot
/// unit never ran. That leaves the previous boot's report in place,
/// which reads as a perfectly healthy boot unless something compares
/// the recorded kernel boot id against the live one — and it is not a
/// hypothetical: a systemd ordering cycle deleted the boot job on 2 of
/// 50 power cuts during Phase 0.
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

#include "einheit/cli/confd/boot_report.h"
#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/confd/config_file.h"
#include "einheit/cli/confd/memory_backend.h"
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
             ("einheit_bootrep_test_" + name + "_" +
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

constexpr const char *kSchemaYaml = R"yaml(
version: 1
product: bootrep-test

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

/// A box whose ReadRunning can be made to disagree with what was
/// applied — that disagreement is the divergence signal.
class DriftBox : public ConfigBackend {
 public:
  explicit DriftBox(std::shared_ptr<const schema::Schema> schema)
      : schema_(std::move(schema)) {}

  auto Apply(const Candidate &candidate)
      -> std::expected<CommitId, Error<ApplyError>> override {
    std::lock_guard<std::mutex> lk(mu_);
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
    for (const auto &[k, v] : overrides_) out[k] = v;
    return out;
  }

  auto Schema() const -> const schema::Schema & override {
    return schema_.Get();
  }

  /// Make the box report `value` at `path` no matter what was applied.
  auto Override(const std::string &path, const std::string &value) -> void {
    std::lock_guard<std::mutex> lk(mu_);
    overrides_[path] = value;
  }

  auto Blank() -> void {
    std::lock_guard<std::mutex> lk(mu_);
    programmed_.clear();
  }

  auto FailNextApply() -> void {
    std::lock_guard<std::mutex> lk(mu_);
    fail_next_ = true;
  }

 private:
  schema::SchemaHandle schema_;
  mutable std::mutex mu_;
  Config programmed_;
  Config overrides_;
  CommitId rev_ = 0;
  bool fail_next_ = false;
};

struct Fixture {
  TempDir dir;
  DriftBox box{TestSchema()};
  std::optional<Runtime> rt;

  explicit Fixture(const std::string &name) : dir(name) {}

  auto Start(const std::string &factory = {}) -> void {
    RuntimeOptions opts;
    opts.state_dir = dir.str();
    opts.factory_config = factory;
    rt.emplace(box, opts);
  }

  auto Commit(const std::string &path, const std::string &value) -> void {
    auto opened = rt->HandleRequest(Req("configure"));
    EXPECT_TRUE(Ok(opened));
    const auto s = Body(opened);
    EXPECT_TRUE(Ok(rt->HandleRequest(Req("set", {path, value}, s))));
    EXPECT_TRUE(Ok(rt->HandleRequest(Req("commit", {}, s))));
  }

  auto Reboot() -> void {
    rt.reset();
    box.Blank();
    Start();
  }
};

// ── The report file ─────────────────────────────────────────────

TEST(BootReport, MissingReportIsNotAnError) {
  TempDir dir("missing");
  auto r = LoadBootReport(dir.str());
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r->has_value());
}

TEST(BootReport, RoundTripsEveryField) {
  TempDir dir("roundtrip");
  BootReport w;
  w.boot_id = "abc-123";
  w.timestamp = "2026-07-26T18:00:00Z";
  w.ok = false;
  w.applied_revision = 42;
  w.paths = 17;
  w.seeded_factory = true;
  w.reverted_pending = true;
  w.reconcile_added = 3;
  w.reconcile_conflicts = 2;
  w.duration_ms = 1234;
  w.steps.push_back({"fabric", true, "br0 up, 4 members", 210});
  w.steps.push_back(
      {"config-apply", false, "enslaving lan1 to br0 failed", 990});
  ASSERT_TRUE(SaveBootReport(dir.str(), w).has_value());

  auto read = LoadBootReport(dir.str());
  ASSERT_TRUE(read.has_value());
  ASSERT_TRUE(read->has_value());
  const auto &r = **read;
  EXPECT_EQ(r.boot_id, "abc-123");
  EXPECT_EQ(r.timestamp, "2026-07-26T18:00:00Z");
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.applied_revision, 42u);
  EXPECT_EQ(r.paths, 17u);
  EXPECT_TRUE(r.seeded_factory);
  EXPECT_TRUE(r.reverted_pending);
  EXPECT_EQ(r.reconcile_added, 3u);
  EXPECT_EQ(r.reconcile_conflicts, 2u);
  EXPECT_EQ(r.duration_ms, 1234);
  ASSERT_EQ(r.steps.size(), 2u);
  EXPECT_EQ(r.steps[0].name, "fabric");
  EXPECT_TRUE(r.steps[0].ok);
  EXPECT_EQ(r.steps[0].duration_ms, 210);
  EXPECT_EQ(r.steps[1].name, "config-apply");
  EXPECT_FALSE(r.steps[1].ok);
  // Step details are free text with spaces; they must survive whole.
  EXPECT_EQ(r.steps[1].detail, "enslaving lan1 to br0 failed");
}

TEST(BootReport, ThisBootTestNeedsBothIdsAndAMatch) {
  BootReport r;
  r.boot_id = "aaa";
  EXPECT_TRUE(IsFromCurrentBoot(r, "aaa"));
  EXPECT_FALSE(IsFromCurrentBoot(r, "bbb"));
  // "Cannot prove it ran this boot" must read as no, never as yes.
  EXPECT_FALSE(IsFromCurrentBoot(r, ""));
  r.boot_id.clear();
  EXPECT_FALSE(IsFromCurrentBoot(r, "aaa"));
  EXPECT_FALSE(IsFromCurrentBoot(r, ""));
}

TEST(BootReport, CurrentBootIdIsAPlainIdentifierOrEmpty) {
  const auto id = CurrentBootId();
  for (const char c : id) {
    EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)) != 0 ||
                c == '-')
        << "boot id must be safe to render: " << id;
  }
}

// ── The runtime writes it ───────────────────────────────────────

TEST(BootReport, ARestoreIsRecorded) {
  Fixture f("restore");
  f.Start();
  f.Commit("name", "sw-1");
  f.Reboot();

  BootStep fabric{"fabric", true, "br0 up, 4 members enslaved", 12};
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot({fabric}).has_value());

  auto rep = f.rt->LastBootReport();
  ASSERT_TRUE(rep.has_value());
  EXPECT_TRUE(rep->ok);
  EXPECT_EQ(rep->applied_revision, 1u);
  EXPECT_EQ(rep->paths, 1u);
  EXPECT_FALSE(rep->boot_id.empty());
  // The product's step comes first, then the framework's — the report
  // reads in the order the boot actually happened.
  ASSERT_EQ(rep->steps.size(), 2u);
  EXPECT_EQ(rep->steps[0].name, "fabric");
  EXPECT_EQ(rep->steps[1].name, "config-apply");
  EXPECT_TRUE(rep->steps[1].ok);
}

TEST(BootReport, AFailedApplyIsStillRecorded) {
  Fixture f("failed");
  f.Start();
  f.Commit("name", "sw-1");
  f.Reboot();
  f.box.FailNextApply();
  ASSERT_FALSE(f.rt->ApplyRunningAtBoot().has_value());

  // "The apply failed" is exactly the outcome an operator needs after
  // the fact — a report only written on success would hide it.
  auto rep = f.rt->LastBootReport();
  ASSERT_TRUE(rep.has_value());
  EXPECT_FALSE(rep->ok);
  ASSERT_EQ(rep->steps.size(), 1u);
  EXPECT_FALSE(rep->steps[0].ok);
  EXPECT_NE(rep->steps[0].detail.find("rejection"), std::string::npos);
}

TEST(BootReport, AFailedProductStepMakesTheBootNotOk) {
  Fixture f("fabric_failed");
  f.Start();
  f.Commit("name", "sw-1");
  f.Reboot();
  // A switch whose fabric did not come up has not had a good boot, even
  // though the configuration applied cleanly afterwards.
  BootStep fabric{"fabric", false, "creating bridge br0 failed", 5};
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot({fabric}).has_value());
  auto rep = f.rt->LastBootReport();
  ASSERT_TRUE(rep.has_value());
  EXPECT_FALSE(rep->ok);
  EXPECT_TRUE(rep->steps[1].ok);  // the apply itself was fine
}

TEST(BootReport, DivergenceIsCountedNotSilentlyAccepted) {
  Fixture f("divergence");
  f.Start();
  f.Commit("name", "committed");
  f.Reboot();
  // The box insists on a different value for a path we just wrote:
  // something outside the management plane changed it.
  f.box.Override("name", "changed-behind-our-back");
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());

  auto rep = f.rt->LastBootReport();
  ASSERT_TRUE(rep.has_value());
  EXPECT_EQ(rep->reconcile_conflicts, 1u);
  // Intent still wins in running: the operator's committed value is
  // what we report, the disagreement is what we flag.
  EXPECT_EQ(f.rt->Running().at("name"), "committed");
}

TEST(BootReport, AgreementCountsNoDivergence) {
  Fixture f("no_divergence");
  f.Start();
  f.Commit("name", "committed");
  f.Reboot();
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());
  auto rep = f.rt->LastBootReport();
  ASSERT_TRUE(rep.has_value());
  EXPECT_EQ(rep->reconcile_conflicts, 0u);
}

TEST(BootReport, BoxOnlyPathsCountAsAddedNotAsDivergence) {
  Fixture f("added");
  f.Start();
  f.Commit("name", "committed");
  f.Reboot();
  // The box always knows things the configuration does not say; that is
  // routine and must not raise an alarm.
  f.box.Override("port", "7");
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());
  auto rep = f.rt->LastBootReport();
  ASSERT_TRUE(rep.has_value());
  EXPECT_EQ(rep->reconcile_added, 1u);
  EXPECT_EQ(rep->reconcile_conflicts, 0u);
}

TEST(BootReport, SurvivesTheProcessThatWroteIt) {
  Fixture f("survives");
  f.Start();
  f.Commit("name", "sw-1");
  f.Reboot();
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());

  // The process that serves `show system boot` is an interactive CLI
  // that never ran a boot apply, so the report has to come off disk.
  f.rt.reset();
  f.Start();
  auto rep = f.rt->LastBootReport();
  ASSERT_TRUE(rep.has_value());
  EXPECT_EQ(rep->applied_revision, 1u);
}

// ── show system boot ────────────────────────────────────────────

TEST(ShowSystemBoot, ReportsARestoreAsThisBoot) {
  Fixture f("show_ok");
  f.Start();
  f.Commit("name", "sw-1");
  f.Reboot();
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());

  const auto body = Body(f.rt->HandleRequest(Req("show_system_boot")));
  EXPECT_NE(body.find("ran_this_boot=yes"), std::string::npos) << body;
  EXPECT_NE(body.find("outcome=ok"), std::string::npos) << body;
  EXPECT_NE(body.find("applied_revision=1"), std::string::npos) << body;
  EXPECT_NE(body.find("step.config-apply=ok"), std::string::npos) << body;
  EXPECT_EQ(body.find("did NOT run"), std::string::npos) << body;
}

TEST(ShowSystemBoot, ABootWhereTheUnitNeverRanSaysExactlyThat) {
  // The systemd-ordering-cycle class. The report on disk is from an
  // earlier boot; without the boot-id comparison this renders as a
  // healthy boot and the operator has no way to tell.
  Fixture f("show_stale");
  f.Start();
  f.Commit("name", "sw-1");
  f.Reboot();
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());

  auto rep = f.rt->LastBootReport();
  ASSERT_TRUE(rep.has_value());
  rep->boot_id = "a-previous-boot-id";
  ASSERT_TRUE(SaveBootReport(f.dir.str(), *rep).has_value());

  const auto body = Body(f.rt->HandleRequest(Req("show_system_boot")));
  EXPECT_NE(body.find("ran_this_boot=no"), std::string::npos) << body;
  EXPECT_NE(body.find("EARLIER boot"), std::string::npos) << body;
  // And it must point somewhere useful, not just say "no".
  EXPECT_NE(body.find("hint="), std::string::npos) << body;
}

TEST(ShowSystemBoot, NoReportAtAllIsStated) {
  Fixture f("show_none");
  f.Start();
  const auto body = Body(f.rt->HandleRequest(Req("show_system_boot")));
  EXPECT_NE(body.find("never run"), std::string::npos) << body;
}

TEST(ShowSystemBoot, SurfacesDivergenceAndFactorySeeding) {
  Fixture f("show_detail");
  const auto factory = (f.dir.path / "factory.conf").string();
  ASSERT_TRUE(
      WriteConfigFile(factory, {{"name", "einheit-s5"}}).has_value());
  f.Start(factory);
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());
  auto body = Body(f.rt->HandleRequest(Req("show_system_boot")));
  EXPECT_NE(body.find("seeded_factory=yes"), std::string::npos) << body;

  f.Reboot();
  f.box.Override("name", "changed-outside");
  ASSERT_TRUE(f.rt->ApplyRunningAtBoot().has_value());
  body = Body(f.rt->HandleRequest(Req("show_system_boot")));
  EXPECT_NE(body.find("config_divergence=1"), std::string::npos) << body;
}

TEST(ShowSystemBoot, ReportsAFailedBootAsFailed) {
  Fixture f("show_failed");
  f.Start();
  f.Commit("name", "sw-1");
  f.Reboot();
  f.box.FailNextApply();
  ASSERT_FALSE(f.rt->ApplyRunningAtBoot().has_value());
  const auto body = Body(f.rt->HandleRequest(Req("show_system_boot")));
  EXPECT_NE(body.find("outcome=FAILED"), std::string::npos) << body;
  EXPECT_NE(body.find("step.config-apply=FAILED"), std::string::npos)
      << body;
}

TEST(ShowSystemBoot, WorksWithoutAStateDirectory) {
  // In-memory runtimes have no persisted report; the verb must still
  // answer rather than error.
  MemoryBackend backend(TestSchema());
  Runtime rt(backend, {});
  auto before = Body(rt.HandleRequest(Req("show_system_boot")));
  EXPECT_NE(before.find("never run"), std::string::npos) << before;
  ASSERT_TRUE(rt.ApplyRunningAtBoot().has_value());
  auto after = Body(rt.HandleRequest(Req("show_system_boot")));
  EXPECT_NE(after.find("outcome=ok"), std::string::npos) << after;
}

}  // namespace
}  // namespace einheit::cli::confd
