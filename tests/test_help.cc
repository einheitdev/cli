/// @file test_help.cc
/// @brief Tests for FormatHelpIndex + FormatCommandHelp.
// Copyright (c) 2026 Einheit Networks

#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/globals.h"

namespace einheit::cli {

TEST(Help, IndexListsEveryPath) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  const auto txt = FormatHelpIndex(tree);
  EXPECT_NE(txt.find("show config"), std::string::npos);
  EXPECT_NE(txt.find("commit"), std::string::npos);
  EXPECT_NE(txt.find("exit"), std::string::npos);
  // Sorted: "alias" precedes "commit".
  EXPECT_LT(txt.find("alias"), txt.find("commit"));
}

TEST(Help, PerCommandShowsRoleAndArgs) {
  CommandSpec s;
  s.path = "restart tunnel";
  s.wire_command = "restart_tunnel";
  s.help = "Restart a single tunnel by name";
  s.role = RoleGate::OperatorOrAdmin;
  ArgSpec name;
  name.name = "name";
  name.help = "Tunnel name";
  name.required = true;
  s.args.push_back(name);
  s.flags.push_back("force");

  const auto txt = FormatCommandHelp(s);
  EXPECT_NE(txt.find("restart tunnel"), std::string::npos);
  EXPECT_NE(txt.find("Restart a single tunnel"),
            std::string::npos);
  EXPECT_NE(txt.find("role: operator"), std::string::npos);
  EXPECT_NE(txt.find("<name>"), std::string::npos);
  EXPECT_NE(txt.find("--force"), std::string::npos);
}

TEST(Help, SessionRequiringCommandNoted) {
  CommandSpec s;
  s.path = "commit";
  s.wire_command = "commit";
  s.role = RoleGate::AdminOnly;
  s.requires_session = true;
  const auto txt = FormatCommandHelp(s);
  EXPECT_NE(txt.find("configure session"), std::string::npos);
  EXPECT_NE(txt.find("role: admin"), std::string::npos);
}

}  // namespace einheit::cli
