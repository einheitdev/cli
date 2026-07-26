/// @file test_confd_config_file.cc
/// @brief Config-file surface: the flat-kv format itself, and the
/// save / load merge|replace / load factory verbs over a Runtime.
// Copyright (c) 2026 Einheit Networks

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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
             ("einheit_cfgfile_test_" + name + "_" +
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

constexpr const char *kSchemaYaml = R"yaml(
version: 1
product: cfgfile-test

config:
  name:
    type: string
    help: "Free string"
  port:
    type: integer
    range: [1, 100]
    help: "Small int"
  mode:
    type: enum
    values: [alpha, beta]
    help: "Enum"

types: {}
)yaml";

auto TestSchema() -> std::shared_ptr<const schema::Schema> {
  auto s = schema::LoadSchemaFromString(kSchemaYaml);
  return s ? *s : std::make_shared<const schema::Schema>();
}

// A Runtime with a state dir plus a shipped factory-defaults file.
struct Fixture {
  TempDir dir;
  MemoryBackend backend{TestSchema()};
  std::optional<Runtime> rt;

  explicit Fixture(const std::string &name) : dir(name) {}

  auto Start(const std::string &factory = {}) -> void {
    RuntimeOptions opts;
    opts.state_dir = dir.str();
    opts.factory_config = factory;
    rt.emplace(backend, opts);
  }

  // Write a factory-defaults file and return its path.
  auto WriteFactory(const Config &values) -> std::string {
    const auto path = (dir.path / "factory.conf").string();
    EXPECT_TRUE(WriteConfigFile(path, values).has_value());
    return path;
  }

  auto Configure() -> std::string {
    auto r = rt->HandleRequest(Req("configure"));
    EXPECT_TRUE(Ok(r));
    return Body(r);
  }
};

// ── The file format ─────────────────────────────────────────────

TEST(ConfigFile, NameValidationRejectsTraversalAndSeparators) {
  EXPECT_TRUE(ValidConfigName("nightly"));
  EXPECT_TRUE(ValidConfigName("golden-2026.07.26_v2"));
  // The whole point of name-not-path: `load replace` runs as root.
  EXPECT_FALSE(ValidConfigName(".."));
  EXPECT_FALSE(ValidConfigName("../../etc/shadow"));
  EXPECT_FALSE(ValidConfigName("/etc/shadow"));
  EXPECT_FALSE(ValidConfigName("sub/dir"));
  EXPECT_FALSE(ValidConfigName(".hidden"));
  EXPECT_FALSE(ValidConfigName("has space"));
  EXPECT_FALSE(ValidConfigName("semi;colon"));
  EXPECT_FALSE(ValidConfigName(""));
  EXPECT_FALSE(ValidConfigName(std::string(65, 'a')));
}

TEST(ConfigFile, ResolvesNamesInsideTheConfigDirectoryOnly) {
  auto good = ConfigFilePath("/var/lib/x", "nightly");
  ASSERT_TRUE(good.has_value());
  EXPECT_EQ(*good, "/var/lib/x/nightly.conf");

  auto bad = ConfigFilePath("/var/lib/x", "../../etc/shadow");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code, ConfigFileError::BadName);

  auto no_dir = ConfigFilePath("", "nightly");
  ASSERT_FALSE(no_dir.has_value());
  EXPECT_EQ(no_dir.error().code, ConfigFileError::NoConfigDir);
}

TEST(ConfigFile, RoundTripsAndIsByteStableAcrossSaves) {
  TempDir dir("roundtrip");
  const auto path = (dir.path / "a.conf").string();
  const Config values = {
      {"name", "sw-1"}, {"port", "42"}, {"mode", "alpha"}};
  ASSERT_TRUE(WriteConfigFile(path, values).has_value());
  auto read = ReadConfigFile(path);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(*read, values);

  // Sorted output means a backup diffs cleanly against the next one
  // instead of reshuffling with the hash order.
  std::ifstream f1(path);
  const std::string first((std::istreambuf_iterator<char>(f1)),
                          std::istreambuf_iterator<char>());
  const auto path2 = (dir.path / "b.conf").string();
  ASSERT_TRUE(WriteConfigFile(path2, values).has_value());
  std::ifstream f2(path2);
  const std::string second((std::istreambuf_iterator<char>(f2)),
                           std::istreambuf_iterator<char>());
  EXPECT_EQ(first, second);
  EXPECT_EQ(first.find(kConfigFileHeader), 0u);
}

TEST(ConfigFile, CommentsBlankLinesAndCrlfAreTolerated) {
  TempDir dir("comments");
  const auto path = (dir.path / "hand-edited.conf").string();
  std::ofstream(path) << "# einheit-config v1\n"
                         "\n"
                         "# a comment\n"
                         "name sw-1\r\n"
                         "port 42\n";
  auto read = ReadConfigFile(path);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->at("name"), "sw-1");
  EXPECT_EQ(read->at("port"), "42");
  EXPECT_EQ(read->size(), 2u);
}

TEST(ConfigFile, MalformedLineIsRejectedWithItsLineNumber) {
  TempDir dir("malformed");
  const auto path = (dir.path / "broken.conf").string();
  std::ofstream(path) << "# einheit-config v1\nname sw-1\nno-value-here\n";
  auto read = ReadConfigFile(path);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().code, ConfigFileError::ParseFailed);
  EXPECT_NE(read.error().message.find(":3:"), std::string::npos);
}

TEST(ConfigFile, MissingFileIsNotFound) {
  TempDir dir("missing");
  auto read = ReadConfigFile((dir.path / "nope.conf").string());
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error().code, ConfigFileError::NotFound);
}

TEST(ConfigFile, ListsOnlyConfigFilesSorted) {
  TempDir dir("list");
  ASSERT_TRUE(
      WriteConfigFile((dir.path / "zulu.conf").string(), {}).has_value());
  ASSERT_TRUE(
      WriteConfigFile((dir.path / "alpha.conf").string(), {}).has_value());
  std::ofstream(dir.path / "notes.txt") << "ignore me\n";
  const auto names = ListConfigFiles(dir.str());
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[0], "alpha");
  EXPECT_EQ(names[1], "zulu");
  EXPECT_TRUE(ListConfigFiles((dir.path / "nope").string()).empty());
}

// ── The verbs ───────────────────────────────────────────────────

TEST(ConfdConfigFile, SaveThenLoadReplaceRestoresAConfiguration) {
  Fixture f("save_restore");
  f.Start();
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "golden"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"port", "7"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));

  auto saved = f.rt->HandleRequest(Req("save", {"golden"}));
  ASSERT_TRUE(Ok(saved));
  EXPECT_NE(Body(saved).find("saved=golden"), std::string::npos);

  // Drift away from the saved configuration...
  auto s2 = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "drifted"}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("delete", {"port"}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "drifted");

  // ...and restore it through the normal candidate + commit path.
  auto s3 = f.Configure();
  auto loaded =
      f.rt->HandleRequest(Req("load_replace", {"golden"}, s3));
  ASSERT_TRUE(Ok(loaded));
  EXPECT_NE(Body(loaded).find("paths=2"), std::string::npos);
  // Loading only stages: the box is untouched until commit.
  EXPECT_EQ(f.backend.DeviceState().at("name"), "drifted");
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s3))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "golden");
  EXPECT_EQ(f.backend.DeviceState().at("port"), "7");
}

TEST(ConfdConfigFile, SavedFileSurvivesARestart) {
  Fixture f("survives_restart");
  f.Start();
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "backup-me"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("save", {"nightly"}))));

  f.rt.reset();
  f.backend.ResetDevice();
  f.Start();
  auto listed = Body(f.rt->HandleRequest(Req("show_configs")));
  EXPECT_NE(listed.find("saved=nightly"), std::string::npos);
  auto s2 = f.Configure();
  ASSERT_TRUE(
      Ok(f.rt->HandleRequest(Req("load_replace", {"nightly"}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "backup-me");
}

TEST(ConfdConfigFile, LoadMergeKeepsPathsTheFileDoesNotMention) {
  Fixture f("merge");
  f.Start();
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"port", "9"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));
  // A file that names only `name`.
  ASSERT_TRUE(WriteConfigFile(
                  (f.dir.path / "configs" / "partial.conf").string(),
                  {{"name", "merged"}})
                  .has_value());

  auto s2 = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("load_merge", {"partial"}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "merged");
  EXPECT_EQ(f.backend.DeviceState().at("port"), "9");
}

TEST(ConfdConfigFile, LoadReplaceDropsPathsTheFileDoesNotMention) {
  Fixture f("replace_drops");
  f.Start();
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"port", "9"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));
  ASSERT_TRUE(WriteConfigFile(
                  (f.dir.path / "configs" / "only-name.conf").string(),
                  {{"name", "replaced"}})
                  .has_value());

  auto s2 = f.Configure();
  ASSERT_TRUE(
      Ok(f.rt->HandleRequest(Req("load_replace", {"only-name"}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "replaced");
  EXPECT_FALSE(f.backend.DeviceState().contains("port"));
}

TEST(ConfdConfigFile, LoadNeedsAConfigureSession) {
  Fixture f("needs_session");
  f.Start();
  ASSERT_TRUE(WriteConfigFile(
                  (f.dir.path / "configs" / "x.conf").string(),
                  {{"name", "n"}})
                  .has_value());
  auto r = f.rt->HandleRequest(Req("load_replace", {"x"}));
  ASSERT_FALSE(Ok(r));
  EXPECT_EQ(r.error->code, "no_session");
}

TEST(ConfdConfigFile, AnInvalidFileIsRejectedWholesale) {
  Fixture f("validation");
  f.Start();
  // port 999 is out of the schema's range; name is fine. Neither may
  // land — a half-loaded candidate is worse than a refused one.
  ASSERT_TRUE(WriteConfigFile(
                  (f.dir.path / "configs" / "bad.conf").string(),
                  {{"name", "ok-value"}, {"port", "999"}})
                  .has_value());
  auto s = f.Configure();
  auto r = f.rt->HandleRequest(Req("load_replace", {"bad"}, s));
  ASSERT_FALSE(Ok(r));
  EXPECT_EQ(r.error->code, "validation");
  auto diff = Body(f.rt->HandleRequest(Req("show_diff", {}, s)));
  EXPECT_NE(diff.find("nothing to commit"), std::string::npos);
}

TEST(ConfdConfigFile, SaveAndLoadRejectPathTraversal) {
  Fixture f("traversal");
  f.Start();
  auto bad_save = f.rt->HandleRequest(Req("save", {"../../etc/shadow"}));
  ASSERT_FALSE(Ok(bad_save));
  ASSERT_TRUE(bad_save.error.has_value());
  EXPECT_EQ(bad_save.error->code, "bad_args");
  EXPECT_FALSE(fs::exists("/etc/shadow.conf"));

  auto s = f.Configure();
  auto bad_load =
      f.rt->HandleRequest(Req("load_replace", {"../../etc/passwd"}, s));
  ASSERT_FALSE(Ok(bad_load));
  EXPECT_EQ(bad_load.error->code, "bad_args");
}

TEST(ConfdConfigFile, LoadOfAMissingConfigIsNotFound) {
  Fixture f("missing_load");
  f.Start();
  auto s = f.Configure();
  auto r = f.rt->HandleRequest(Req("load_replace", {"nope"}, s));
  ASSERT_FALSE(Ok(r));
  EXPECT_EQ(r.error->code, "not_found");
}

TEST(ConfdConfigFile, FactoryLoadStagesTheShippedDefaults) {
  Fixture f("factory");
  const auto factory =
      f.WriteFactory({{"name", "factory-default"}, {"mode", "alpha"}});
  f.Start(factory);
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"port", "77"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));

  auto s2 = f.Configure();
  auto loaded = f.rt->HandleRequest(Req("load_factory", {}, s2));
  ASSERT_TRUE(Ok(loaded));
  EXPECT_NE(Body(loaded).find("mode=factory"), std::string::npos);
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));
  // Factory reset = replace, so the operator's port is gone.
  EXPECT_EQ(f.backend.DeviceState().at("name"), "factory-default");
  EXPECT_FALSE(f.backend.DeviceState().contains("port"));
}

TEST(ConfdConfigFile, FactoryWithNoShippedFileIsTheEmptyConfiguration) {
  Fixture f("factory_empty");
  f.Start();
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "configured"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));

  auto s2 = f.Configure();
  auto loaded = f.rt->HandleRequest(Req("load_factory", {}, s2));
  ASSERT_TRUE(Ok(loaded));
  EXPECT_NE(Body(loaded).find("paths=0"), std::string::npos);
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));
  EXPECT_TRUE(f.backend.DeviceState().empty());
}

TEST(ConfdConfigFile, ShowConfigsListsSavedFilesAndTheFactoryFile) {
  Fixture f("show_configs");
  const auto factory = f.WriteFactory({{"name", "d"}});
  f.Start(factory);
  auto empty = Body(f.rt->HandleRequest(Req("show_configs")));
  EXPECT_NE(empty.find("saved=<none>"), std::string::npos);
  EXPECT_NE(empty.find("factory=" + factory), std::string::npos);

  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("save", {"one"}))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("save", {"two"}))));
  auto listed = Body(f.rt->HandleRequest(Req("show_configs")));
  EXPECT_NE(listed.find("saved=one"), std::string::npos);
  EXPECT_NE(listed.find("saved=two"), std::string::npos);
  EXPECT_EQ(listed.find("saved=<none>"), std::string::npos);
}

// ── WP0.5 rescue configuration ──────────────────────────────────

TEST(ConfdRescue, SaveRescueAndRollbackRescueRoundTrip) {
  Fixture f("rescue_roundtrip");
  f.Start();
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "known-good"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"port", "22"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("save", {"rescue"}))));

  // Now wreck the box the way an operator would: a hostile config that
  // commits cleanly and leaves the switch unusable.
  auto s2 = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "hostile"}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("delete", {"port"}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "hostile");

  // One verb, no session, applies immediately — this is the verb you
  // reach for when the box is broken.
  auto rolled = f.rt->HandleRequest(Req("rollback_rescue"));
  ASSERT_TRUE(Ok(rolled)) << (rolled.error ? rolled.error->message : "");
  EXPECT_NE(Body(rolled).find("commit_id="), std::string::npos);
  EXPECT_EQ(f.backend.DeviceState().at("name"), "known-good");
  EXPECT_EQ(f.backend.DeviceState().at("port"), "22");
}

TEST(ConfdRescue, RescueLivesOutsideTheConfigsDirectory) {
  // So a factory reset that clears saved configurations cannot take the
  // rescue config with it — surviving a reset is its whole purpose.
  Fixture f("rescue_location");
  f.Start();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("save", {"rescue"}))));
  EXPECT_TRUE(fs::exists(f.dir.path / "rescue.conf"));
  EXPECT_FALSE(fs::exists(f.dir.path / "configs" / "rescue.conf"));
  // And it is not listed among the ordinary saved configs.
  const auto listed = Body(f.rt->HandleRequest(Req("show_configs")));
  EXPECT_NE(listed.find("rescue=saved"), std::string::npos) << listed;
  EXPECT_EQ(listed.find("saved=rescue"), std::string::npos) << listed;
}

TEST(ConfdRescue, RescueSurvivesAFactoryReset) {
  Fixture f("rescue_survives_factory");
  const auto factory = f.WriteFactory({{"name", "factory-default"}});
  f.Start(factory);
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "known-good"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("save", {"rescue"}))));

  auto s2 = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("load_factory", {}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "factory-default");

  // The rescue slot is still there, and still gets the box back.
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("rollback_rescue"))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "known-good");
}

TEST(ConfdRescue, RollbackRescueWithNothingSavedIsAClearError) {
  Fixture f("rescue_missing");
  f.Start();
  auto r = f.rt->HandleRequest(Req("rollback_rescue"));
  ASSERT_FALSE(Ok(r));
  EXPECT_EQ(r.error->code, "not_found");
  EXPECT_NE(r.error->hint.find("save rescue"), std::string::npos);
}

TEST(ConfdRescue, RescueSurvivesARestart) {
  Fixture f("rescue_restart");
  f.Start();
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "known-good"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("save", {"rescue"}))));

  f.rt.reset();
  f.backend.ResetDevice();
  f.Start();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("rollback_rescue"))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "known-good");
}

TEST(ConfdRescue, LoadReplaceRescueStagesItInsteadOfApplying) {
  // The reserved name works through the ordinary load verb too, for an
  // operator who would rather review the rescue config before it lands.
  Fixture f("rescue_stage");
  f.Start();
  auto s = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "known-good"}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("save", {"rescue"}))));
  auto s2 = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("set", {"name", "hostile"}, s2))));
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s2))));

  auto s3 = f.Configure();
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("load_replace", {"rescue"}, s3))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "hostile");  // staged only
  ASSERT_TRUE(Ok(f.rt->HandleRequest(Req("commit", {}, s3))));
  EXPECT_EQ(f.backend.DeviceState().at("name"), "known-good");
}

TEST(ConfdRescue, ACorruptRescueConfigIsRefusedNotApplied) {
  Fixture f("rescue_invalid");
  f.Start();
  // port 999 is out of the schema's range.
  ASSERT_TRUE(WriteConfigFile((f.dir.path / "rescue.conf").string(),
                              {{"port", "999"}})
                  .has_value());
  auto r = f.rt->HandleRequest(Req("rollback_rescue"));
  ASSERT_FALSE(Ok(r));
  EXPECT_EQ(r.error->code, "validation");
  EXPECT_EQ(f.backend.ApplyCount(), 0);
}

TEST(ConfdConfigFile, SaveIsUnavailableWithoutAStateDirectory) {
  MemoryBackend backend(TestSchema());
  Runtime rt(backend, {});
  auto r = rt.HandleRequest(Req("save", {"nightly"}));
  ASSERT_FALSE(Ok(r));
  EXPECT_EQ(r.error->code, "unavailable");
}

}  // namespace
}  // namespace einheit::cli::confd
