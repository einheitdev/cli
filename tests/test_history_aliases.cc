/// @file test_history_aliases.cc
/// @brief History + aliases exercised against a tmpdir.
// Copyright (c) 2026 Einheit Networks

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "einheit/cli/aliases.h"
#include "einheit/cli/history.h"

namespace einheit::cli {
namespace {

class TmpDir {
 public:
  TmpDir() {
    path_ =
        std::filesystem::temp_directory_path() /
        ("einheit_tests_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter_++));
    std::filesystem::create_directories(path_);
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  auto Path() const -> std::string { return path_.string(); }

 private:
  static inline int counter_ = 0;
  std::filesystem::path path_;
};

}  // namespace

TEST(History, AppendRoundTrip) {
  TmpDir d;
  auto h = Load("tester", d.Path());
  ASSERT_TRUE(h.has_value());
  EXPECT_TRUE(h->entries.empty());

  ASSERT_TRUE(Append(*h, "show tunnels").has_value());
  ASSERT_TRUE(Append(*h, "configure").has_value());

  auto reloaded = Load("tester", d.Path());
  ASSERT_TRUE(reloaded.has_value());
  ASSERT_EQ(reloaded->entries.size(), 2u);
  EXPECT_EQ(reloaded->entries[0], "show tunnels");
  EXPECT_EQ(reloaded->entries[1], "configure");
}

TEST(History, RotatesWhenOverMax) {
  TmpDir d;
  auto h = Load("tester", d.Path());
  ASSERT_TRUE(h.has_value());
  h->max_entries = 3;

  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(Append(*h, "cmd-" + std::to_string(i)).has_value());
  }
  // Only the last 3 entries should remain in-memory.
  ASSERT_EQ(h->entries.size(), 3u);
  EXPECT_EQ(h->entries.front(), "cmd-2");
  EXPECT_EQ(h->entries.back(), "cmd-4");
}

TEST(Aliases, LoadsAndExpands) {
  TmpDir d;
  std::filesystem::create_directories(d.Path() + "/tester");
  {
    std::ofstream f(d.Path() + "/tester/aliases");
    f << "# comment line\n";
    f << "st=show tunnels\n";
    f << "sr=show routes\n";
  }

  auto a = LoadAliases("tester", d.Path());
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->table.size(), 2u);

  EXPECT_EQ(Expand(*a, "st"), "show tunnels");
  EXPECT_EQ(Expand(*a, "sr"), "show routes");
  // Trailing args survive expansion.
  EXPECT_EQ(Expand(*a, "st munich"), "show tunnels munich");
  // Unknown alias leaves line unchanged.
  EXPECT_EQ(Expand(*a, "show routes"), "show routes");
}

TEST(Aliases, MalformedLinesSkipped) {
  TmpDir d;
  std::filesystem::create_directories(d.Path() + "/tester");
  {
    std::ofstream f(d.Path() + "/tester/aliases");
    f << "no_equals_sign_here\n";
    f << "valid=show status\n";
  }
  auto a = LoadAliases("tester", d.Path());
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->table.size(), 1u);
  EXPECT_EQ(Expand(*a, "valid"), "show status");
}

TEST(AliasesYaml, LoadsBothShapesAndMerges) {
  TmpDir d;
  {
    std::ofstream f(d.Path() + "/aliases.yaml");
    f << R"(
aliases:
  st: show tunnels
  sc:
    expansion: show config
    help: "Dump the running config"
)";
  }
  auto a = LoadAliasesYaml(d.Path() + "/aliases.yaml");
  ASSERT_TRUE(a.has_value()) << a.error().message;
  EXPECT_EQ(a->table["st"], "show tunnels");
  EXPECT_EQ(a->table["sc"], "show config");
  EXPECT_EQ(a->help["sc"], "Dump the running config");
  EXPECT_EQ(Expand(*a, "st munich"), "show tunnels munich");
}

TEST(AliasesYaml, IncludesAreMerged) {
  TmpDir d;
  {
    std::ofstream f(d.Path() + "/base.yaml");
    f << R"(
aliases:
  st: show tunnels
  common: base value
)";
  }
  {
    std::ofstream f(d.Path() + "/user.yaml");
    f << std::format(R"(
include:
  - {}/base.yaml
aliases:
  common: user override
  mine: show routes
)",
                     d.Path());
  }
  auto a = LoadAliasesYaml(d.Path() + "/user.yaml");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->table["st"], "show tunnels");        // from base
  EXPECT_EQ(a->table["common"], "user override");   // overridden
  EXPECT_EQ(a->table["mine"], "show routes");
}

TEST(AliasesYaml, IncludeCycleDoesNotLoop) {
  TmpDir d;
  {
    std::ofstream f(d.Path() + "/a.yaml");
    f << std::format("include:\n  - {}/b.yaml\naliases:\n  a: A\n",
                     d.Path());
  }
  {
    std::ofstream f(d.Path() + "/b.yaml");
    f << std::format("include:\n  - {}/a.yaml\naliases:\n  b: B\n",
                     d.Path());
  }
  auto a = LoadAliasesYaml(d.Path() + "/a.yaml");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->table["a"], "A");
  EXPECT_EQ(a->table["b"], "B");
}

TEST(AliasesYaml, MergeLetsUserOverrideTeam) {
  Aliases team;
  team.table["st"] = "team show";
  Aliases user;
  user.table["st"] = "user show";
  MergeAliases(team, user);
  EXPECT_EQ(team.table["st"], "user show");
}

TEST(AliasesYaml, MissingFileReturnsError) {
  auto a = LoadAliasesYaml("/tmp/absolutely-does-not-exist.yaml");
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error().code, AliasError::NotAccessible);
}

}  // namespace einheit::cli
