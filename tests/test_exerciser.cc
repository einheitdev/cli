/// @file test_exerciser.cc
/// @brief Schema-driven exerciser over MemoryBackend.
// Copyright (c) 2026 Einheit Networks

#include <format>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/confd/exerciser.h"
#include "einheit/cli/confd/memory_backend.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {
namespace {

constexpr const char *kSchemaYaml = R"yaml(
version: 1
product: exerciser-test

config:
  hostname:
    type: string
    help: "Hostname"
  port:
    type: integer
    range: [1, 65535]
    help: "Listen port"
  mode:
    type: enum
    values: [active, standby, off]
    help: "Mode"
  reachable:
    type: boolean
    help: "Flag"
  management_network:
    type: cidr
    help: "Mgmt CIDR"
  gateway:
    type: ip
    help: "Gateway"
  interfaces:
    type: map
    key: string
    value:
      type: object
      fields:
        address:
          type: cidr
          help: "Address"
        vlan:
          type: integer
          range: [1, 4094]
          help: "VLAN tag"

types: {}
)yaml";

auto LoadTestSchema() -> std::shared_ptr<schema::Schema> {
  auto s = schema::LoadSchemaFromString(kSchemaYaml);
  EXPECT_TRUE(s.has_value());
  return *s;
}

TEST(Exerciser, GeneratesTheWholeSurface) {
  const auto schema = LoadTestSchema();
  const auto cases = GenerateCases(*schema);
  // Every leaf appears, map keys substituted.
  bool saw_port_max = false;
  bool saw_vlan = false;
  bool saw_bad_enum = false;
  for (const auto &c : cases) {
    if (c.path == "port" && c.value == "65535" && c.valid) {
      saw_port_max = true;
    }
    if (c.path == "interfaces.k1.vlan") saw_vlan = true;
    if (c.path == "mode" && !c.valid) saw_bad_enum = true;
  }
  EXPECT_TRUE(saw_port_max);
  EXPECT_TRUE(saw_vlan);
  EXPECT_TRUE(saw_bad_enum);
}

TEST(Exerciser, MapKeyHintsAndSkipsApply) {
  const auto schema = LoadTestSchema();
  ExerciseOptions opts;
  opts.map_keys["interfaces"] = "eth0";
  opts.skip_prefixes.push_back("mode");
  const auto cases = GenerateCases(*schema, opts);
  bool saw_eth0 = false;
  for (const auto &c : cases) {
    EXPECT_NE(c.path, "mode");
    if (c.path.rfind("interfaces.eth0.", 0) == 0) saw_eth0 = true;
  }
  EXPECT_TRUE(saw_eth0);
}

TEST(Exerciser, FullSurfacePassesAgainstMemoryBackend) {
  const auto schema = LoadTestSchema();
  MemoryBackend backend(schema);
  Runtime rt(backend);
  const auto cases = GenerateCases(*schema);
  ASSERT_GT(cases.size(), 20u);
  const auto failures = ExerciseRuntime(rt, cases);
  for (const auto &f : failures) {
    ADD_FAILURE() << std::format("{} = '{}' ({}): {}", f.c.path,
                                 f.c.value, f.c.note, f.detail);
  }
  EXPECT_TRUE(failures.empty());
}

TEST(Exerciser, CatchesABackendThatLies) {
  const auto schema = LoadTestSchema();
  MemoryBackend backend(schema);
  Runtime rt(backend);
  // Let the anchor commit land first, THEN arm the fault: the next
  // valid case's commit fails and the exerciser must report it —
  // proving it checks outcomes rather than just driving traffic.
  ASSERT_TRUE(ExerciseRuntime(rt, {}).empty());
  backend.FailNextApply();
  const auto cases = GenerateCases(*schema);
  const auto failures = ExerciseRuntime(rt, cases);
  EXPECT_FALSE(failures.empty());
}

}  // namespace
}  // namespace einheit::cli::confd
