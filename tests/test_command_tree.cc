/// @file test_command_tree.cc
/// @brief Tests for registration, parsing, completion.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include "einheit/cli/command_tree.h"

namespace einheit::cli {

TEST(CommandTree, RegisterAndParse) {
  CommandTree tree;
  CommandSpec show_tunnels;
  show_tunnels.path = "show tunnels";
  show_tunnels.wire_command = "show_tunnels";
  ASSERT_TRUE(Register(tree, show_tunnels).has_value());

  auto parsed =
      Parse(tree, {"show", "tunnels"}, RoleGate::AnyAuthenticated);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->spec->wire_command, "show_tunnels");
}

TEST(CommandTree, UnknownCommand) {
  CommandTree tree;
  auto parsed =
      Parse(tree, {"nope"}, RoleGate::AnyAuthenticated);
  EXPECT_FALSE(parsed.has_value());
}

TEST(CommandTree, UnknownCommandSuggestsDidYouMean) {
  CommandTree tree;
  CommandSpec show;
  show.path = "show tunnels";
  show.wire_command = "show_tunnels";
  ASSERT_TRUE(Register(tree, show).has_value());

  auto parsed = Parse(tree, {"shaw"}, RoleGate::AnyAuthenticated);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().message.find("show"),
            std::string::npos);
}

TEST(CommandTree, PrefixExpandsUniqueVerb) {
  CommandTree tree;
  CommandSpec configure;
  configure.path = "configure";
  configure.wire_command = "configure";
  CommandSpec commit;
  commit.path = "commit";
  commit.wire_command = "commit";
  ASSERT_TRUE(Register(tree, configure).has_value());
  ASSERT_TRUE(Register(tree, commit).has_value());

  // "config" is a unique prefix of "configure".
  auto parsed = Parse(tree, {"config"}, RoleGate::AnyAuthenticated);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->spec->path, "configure");
}

TEST(CommandTree, PrefixExpandsInsideSubcommand) {
  CommandTree tree;
  CommandSpec show_config;
  show_config.path = "show config";
  show_config.wire_command = "show_config";
  CommandSpec show_commits;
  show_commits.path = "show commits";
  show_commits.wire_command = "show_commits";
  ASSERT_TRUE(Register(tree, show_config).has_value());
  ASSERT_TRUE(Register(tree, show_commits).has_value());

  auto parsed = Parse(tree, {"show", "conf"},
                      RoleGate::AnyAuthenticated);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->spec->path, "show config");
}

TEST(CommandTree, AmbiguousPrefixReportsOptions) {
  CommandTree tree;
  CommandSpec commit;
  commit.path = "commit";
  commit.wire_command = "commit";
  CommandSpec commits;
  commits.path = "show commits";
  commits.wire_command = "show_commits";
  ASSERT_TRUE(Register(tree, commit).has_value());
  ASSERT_TRUE(Register(tree, commits).has_value());

  CommandSpec configure;
  configure.path = "configure";
  configure.wire_command = "configure";
  ASSERT_TRUE(Register(tree, configure).has_value());

  // "co" matches both `commit` and `configure` at length 1.
  auto parsed = Parse(tree, {"co"}, RoleGate::AnyAuthenticated);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_NE(parsed.error().message.find("ambiguous"),
            std::string::npos);
  EXPECT_NE(parsed.error().message.find("commit"),
            std::string::npos);
  EXPECT_NE(parsed.error().message.find("configure"),
            std::string::npos);
}

TEST(CommandTree, ExactMatchWinsOverPrefix) {
  CommandTree tree;
  CommandSpec commit;
  commit.path = "commit";
  commit.wire_command = "commit";
  CommandSpec commits;
  commits.path = "show commits";
  commits.wire_command = "show_commits";
  ASSERT_TRUE(Register(tree, commit).has_value());
  ASSERT_TRUE(Register(tree, commits).has_value());

  auto parsed = Parse(tree, {"commit"}, RoleGate::AnyAuthenticated);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->spec->path, "commit");
}

TEST(CommandTree, RoleGate) {
  CommandTree tree;
  CommandSpec configure;
  configure.path = "configure";
  configure.wire_command = "configure";
  configure.role = RoleGate::AdminOnly;
  ASSERT_TRUE(Register(tree, configure).has_value());

  auto denied =
      Parse(tree, {"configure"}, RoleGate::AnyAuthenticated);
  EXPECT_FALSE(denied.has_value());
  EXPECT_EQ(denied.error().code, CommandTreeError::NotAuthorised);

  auto allowed = Parse(tree, {"configure"}, RoleGate::AdminOnly);
  ASSERT_TRUE(allowed.has_value());
}

TEST(CommandTree, Completions) {
  CommandTree tree;
  CommandSpec a, b;
  a.path = "show tunnels";
  a.wire_command = "show_tunnels";
  b.path = "show routes";
  b.wire_command = "show_routes";
  ASSERT_TRUE(Register(tree, a).has_value());
  ASSERT_TRUE(Register(tree, b).has_value());

  auto c = SuggestCompletions(tree, {"show"}, "t");
  ASSERT_EQ(c.size(), 1u);
  EXPECT_EQ(c[0], "tunnels");
}

}  // namespace einheit::cli
