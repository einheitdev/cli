/// @file test_hd_relay_adapter.cc
/// @brief hd-relay adapter — contract, metadata, command surface,
/// schema smoke tests.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>

#include "adapters/hd_relay/adapter.h"
#include "einheit/cli/adapter_contract.h"
#include "einheit/cli/schema.h"

namespace {

TEST(HdRelayAdapter, PassesContractValidator) {
  auto adapter =
      einheit::adapters::hd_relay::NewHdRelayAdapter();
  auto r = einheit::cli::contract::ValidateAdapter(*adapter);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_GT(r->commands_checked, 10u);
  EXPECT_GE(r->topics_checked, 3u);
}

TEST(HdRelayAdapter, MetadataSane) {
  auto adapter =
      einheit::adapters::hd_relay::NewHdRelayAdapter();
  const auto meta = adapter->Metadata();
  EXPECT_EQ(meta.id, "hd-relay");
  EXPECT_FALSE(meta.display_name.empty());
  EXPECT_FALSE(meta.version.empty());
  EXPECT_FALSE(meta.prompt.empty());
  EXPECT_TRUE(adapter->ControlSocketPath().starts_with("ipc://"));
  EXPECT_TRUE(adapter->EventSocketPath().starts_with("ipc://"));
}

TEST(HdRelayAdapter, CommandSurfaceCoversHdCore) {
  auto adapter =
      einheit::adapters::hd_relay::NewHdRelayAdapter();
  const auto cmds = adapter->Commands();
  std::unordered_set<std::string> paths;
  std::unordered_set<std::string> wires;
  for (const auto &c : cmds) {
    paths.insert(c.path);
    if (!c.wire_command.empty()) wires.insert(c.wire_command);
  }
  // Minimum surface the daemon side must be able to answer.
  for (const char *want : {"show status", "show peers",
                            "show peer", "show audit",
                            "show counters", "show config",
                            "show fleet", "peer approve",
                            "peer deny", "peer revoke",
                            "peer redirect", "peer policy set",
                            "peer policy clear",
                            "peer rule add", "relay init",
                            "watch peers", "watch audit",
                            "watch metrics"}) {
    EXPECT_TRUE(paths.count(want)) << "missing path: " << want;
  }
  for (const char *want : {"show_status", "show_peers",
                            "show_peer", "show_audit",
                            "show_counters", "show_config",
                            "show_fleet", "peer_approve",
                            "peer_deny", "peer_revoke",
                            "peer_redirect", "peer_policy_set",
                            "peer_policy_clear",
                            "peer_rule_add", "relay_init"}) {
    EXPECT_TRUE(wires.count(want)) << "missing wire: " << want;
  }
}

TEST(HdRelayAdapter, RoleGatesMakeSense) {
  auto adapter =
      einheit::adapters::hd_relay::NewHdRelayAdapter();
  for (const auto &c : adapter->Commands()) {
    if (c.path.starts_with("peer ") ||
        c.path == "relay init") {
      EXPECT_EQ(c.role,
                einheit::cli::RoleGate::AdminOnly)
          << c.path << " should be admin-gated";
    }
    if (c.path == "show status" || c.path == "show peers") {
      EXPECT_EQ(c.role,
                einheit::cli::RoleGate::AnyAuthenticated);
    }
  }
}

TEST(HdRelayAdapter, SchemaCoversKeyPaths) {
  auto adapter =
      einheit::adapters::hd_relay::NewHdRelayAdapter();
  const auto &s = adapter->GetSchema();
  EXPECT_EQ(s.product, "hd-relay");
  // Spot-check a handful of schema paths that drive validation +
  // completion for the typical operator workflow.
  for (const char *path : {"port", "workers", "log_level",
                            "hd.relay_key", "hd.relay_id",
                            "hd.enroll_mode",
                            "hd.relay_policy.default_mode",
                            "hd.relay_policy.max_direct_peers",
                            "hd.federation.fleet_id",
                            "hd.fleet_controller.url",
                            "hd.audit_log_path",
                            "metrics.port",
                            "level2.enabled"}) {
    auto r = einheit::cli::schema::ValidatePath(s, path, "");
    // Missing paths yield SchemaError::ValidationFailed with a
    // "no such path" message; accept a "required" rejection here
    // and only fail if the framework couldn't walk to the path.
    EXPECT_TRUE(r.has_value() ||
                r.error().message.find("no such path") ==
                    std::string::npos)
        << "schema missing path: " << path
        << " (" << (r.has_value() ? "ok" : r.error().message)
        << ")";
  }
}

TEST(HdRelayAdapter, WatchTopicsFollowStateConvention) {
  auto adapter =
      einheit::adapters::hd_relay::NewHdRelayAdapter();
  for (const auto &c : adapter->Commands()) {
    if (!c.path.starts_with("watch ")) continue;
    const auto topics = adapter->EventTopicsFor(c);
    ASSERT_FALSE(topics.empty()) << c.path;
    for (const auto &t : topics) {
      EXPECT_TRUE(t.starts_with("state.")) << t;
    }
  }
}

}  // namespace
