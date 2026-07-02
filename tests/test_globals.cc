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

// The core utility verbs are always registered on their own — a
// product that opts out of candidate-config must still get help/exit.
TEST(Globals, CoreOnlyStillHasHelpAndExit) {
  CommandTree tree;
  ASSERT_TRUE(RegisterCoreGlobals(tree).has_value());
  EXPECT_NE(tree.by_path.find("help"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("exit"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("quit"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("history"), tree.by_path.end());
  // …but no candidate-config verbs.
  EXPECT_EQ(tree.by_path.find("set"), tree.by_path.end());
  EXPECT_EQ(tree.by_path.find("commit"), tree.by_path.end());
  EXPECT_EQ(tree.by_path.find("configure"), tree.by_path.end());
}

// Opting out of config verbs (a product not ready for candidate-config)
// leaves the core intact and omits set/commit/configure/rollback so
// they can't misreport (gap #6).
TEST(Globals, ConfigVerbsAreOptIn) {
  CommandTree tree;
  ASSERT_TRUE(
      RegisterGlobals(tree, GlobalsOptions{.config_verbs = false})
          .has_value());
  // Core present.
  EXPECT_NE(tree.by_path.find("help"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("doctor"), tree.by_path.end());
  EXPECT_NE(tree.by_path.find("daemon status"), tree.by_path.end());
  // Config absent.
  for (const auto *p :
       {"set", "delete", "commit", "commit confirmed", "confirm",
        "configure", "rollback candidate", "rollback previous",
        "rollback to", "show config", "show commits", "show schema"}) {
    EXPECT_EQ(tree.by_path.find(p), tree.by_path.end()) << p;
  }
}

// The default options overload registers both families — identical to
// the historical all-in RegisterGlobals(tree).
TEST(Globals, DefaultOptionsRegistersCoreAndConfig) {
  CommandTree a;
  CommandTree b;
  ASSERT_TRUE(RegisterGlobals(a).has_value());
  ASSERT_TRUE(RegisterGlobals(b, GlobalsOptions{}).has_value());
  EXPECT_EQ(a.by_path.size(), b.by_path.size());
  for (const auto &[path, _] : a.by_path) {
    EXPECT_NE(b.by_path.find(path), b.by_path.end()) << path;
  }
}

// Core + config together must not collide on any path.
TEST(Globals, CoreAndConfigDoNotOverlap) {
  CommandTree tree;
  ASSERT_TRUE(RegisterCoreGlobals(tree).has_value());
  // Registering config on top must succeed (no duplicate paths).
  EXPECT_TRUE(RegisterConfigGlobals(tree).has_value());
}

}  // namespace einheit::cli
