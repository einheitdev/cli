/// @file test_globals.cc
/// @brief Tests for the framework-global command registration.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/globals.h"

namespace einheit::cli {

TEST(Globals, RegistersExpectedPaths) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());

  EXPECT_NE(tree.by_path.find("show config"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("configure"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("commit"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("rollback candidate"),
            tree.by_path.end());
  EXPECT_NE(tree.by_path.find("rollback previous"),
            tree.by_path.end());
  EXPECT_NE(tree.by_path.find("exit"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("help"), tree.by_path.end());
}

TEST(Globals, ConfigureGuardedByAdmin) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  auto it = tree.by_path.find("configure");
  ASSERT_NE(it, tree.by_path.end());
  EXPECT_EQ(it->second.role, RoleGate::AdminOnly);
}

TEST(Globals, FrameworkLocalHaveEmptyWireCommand) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  for (const auto *path :
       {"exit", "quit", "help", "history", "alias", "watch"}) {
    auto it = tree.by_path.find(path);
    ASSERT_NE(it, tree.by_path.end()) << path;
    EXPECT_TRUE(it->second.wire_command.empty()) << path;
  }
}

TEST(Globals, DuplicateRegistrationRejected) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  auto second = RegisterGlobals(tree);
  EXPECT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code,
            CommandTreeError::DuplicateRegistration);
}

TEST(Globals, AdminParsesAdminOnlyVerbs) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  auto parsed = Parse(tree, {"configure"}, RoleGate::AdminOnly);
  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  EXPECT_EQ(parsed->spec->wire_command, "configure");
}

TEST(Globals, ReadOnlyUserCannotConfigure) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  auto parsed =
      Parse(tree, {"configure"}, RoleGate::AnyAuthenticated);
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().code, CommandTreeError::NotAuthorised);
}

}  // namespace einheit::cli
