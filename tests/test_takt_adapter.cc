/// @file test_takt_adapter.cc
/// @brief takt adapter — contract, metadata, command
/// surface tests.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>

#include "adapters/takt/adapter.h"

namespace {

TEST(TaktAdapter, MetadataSane) {
  auto adapter =
      einheit::adapters::takt::NewTaktAdapter();
  const auto meta = adapter->Metadata();
  EXPECT_EQ(meta.id, "takt");
  EXPECT_FALSE(meta.display_name.empty());
  EXPECT_FALSE(meta.version.empty());
  EXPECT_FALSE(meta.prompt.empty());
  EXPECT_TRUE(
      adapter->ControlSocketPath().starts_with("ipc://"))
      << adapter->ControlSocketPath();
  EXPECT_TRUE(
      adapter->EventSocketPath().starts_with("ipc://"))
      << adapter->EventSocketPath();
}

TEST(TaktAdapter, CommandSurfaceCoversTaktCore) {
  auto adapter =
      einheit::adapters::takt::NewTaktAdapter();
  const auto cmds = adapter->Commands();
  std::unordered_set<std::string> paths;
  for (const auto &c : cmds) {
    paths.insert(c.path);
  }
  for (const char *want : {
           "show workspaces", "show workspace",
           "show targets", "show runs", "show run",
           "show agents", "show pipeline",
           "pipeline run",
           "target claim", "target release",
           "target up", "target down",
           "workspace create", "workspace delete",
           "watch runs", "watch agents"}) {
    EXPECT_TRUE(paths.count(want))
        << "missing path: " << want;
  }
}

TEST(TaktAdapter, CommandsHaveHelp) {
  auto adapter =
      einheit::adapters::takt::NewTaktAdapter();
  for (const auto &cmd : adapter->Commands()) {
    EXPECT_FALSE(cmd.help.empty())
        << "command '" << cmd.path
        << "' missing help";
  }
}

TEST(TaktAdapter, WireCommandsNonEmptyForRpc) {
  auto adapter =
      einheit::adapters::takt::NewTaktAdapter();
  for (const auto &cmd : adapter->Commands()) {
    if (cmd.path.starts_with("watch")) {
      EXPECT_TRUE(cmd.wire_command.empty())
          << "watch '" << cmd.path
          << "' should have empty wire_command";
    } else {
      EXPECT_FALSE(cmd.wire_command.empty())
          << "command '" << cmd.path
          << "' missing wire_command";
    }
  }
}

TEST(TaktAdapter, WatchRunsHasTopics) {
  auto adapter =
      einheit::adapters::takt::NewTaktAdapter();
  for (const auto &cmd : adapter->Commands()) {
    if (cmd.path == "watch runs") {
      auto topics = adapter->EventTopicsFor(cmd);
      EXPECT_FALSE(topics.empty());
      return;
    }
  }
  FAIL() << "watch runs command not found";
}

TEST(TaktAdapter, WatchAgentsHasTopics) {
  auto adapter =
      einheit::adapters::takt::NewTaktAdapter();
  for (const auto &cmd : adapter->Commands()) {
    if (cmd.path == "watch agents") {
      auto topics = adapter->EventTopicsFor(cmd);
      EXPECT_FALSE(topics.empty());
      return;
    }
  }
  FAIL() << "watch agents command not found";
}

}  // namespace
