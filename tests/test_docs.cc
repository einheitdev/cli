/// @file test_docs.cc
/// @brief Generated operator reference structure.
// Copyright (c) 2026 Einheit Networks

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/docs.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::docs {
namespace {

constexpr const char *kSchemaYaml = R"yaml(
version: 1
product: docs-test

config:
  hostname:
    type: string
    help: "Device | hostname"
  port:
    type: integer
    range: [1, 100]
    default: "42"
    help: "A port"
  nets:
    type: map
    key: string
    value:
      type: object
      fields:
        addr:
          type: cidr
          help: "Address"

types: {}
)yaml";

auto Generate() -> std::string {
  auto schema = schema::LoadSchemaFromString(kSchemaYaml);
  EXPECT_TRUE(schema.has_value());
  CommandTree tree;
  EXPECT_TRUE(RegisterGlobals(tree, GlobalsOptions{}).has_value());
  ProductMetadata meta;
  meta.id = "docs-test";
  meta.display_name = "Docs Test";
  meta.version = "1.2.3";
  return GenerateReference(meta, tree, **schema);
}

TEST(Docs, CoversEverySchemaPathAndCommand) {
  const auto md = Generate();
  EXPECT_NE(md.find("`hostname`"), std::string::npos);
  EXPECT_NE(md.find("`port`"), std::string::npos);
  // Map keys render as a placeholder the operator fills in.
  EXPECT_NE(md.find("nets"), std::string::npos);
  // Range and default surface.
  EXPECT_NE(md.find("42"), std::string::npos);
  // Lifecycle verbs classified into their section.
  const auto lifecycle = md.find("## Configuration lifecycle");
  const auto shows = md.find("## Show commands");
  ASSERT_NE(lifecycle, std::string::npos);
  ASSERT_NE(shows, std::string::npos);
  EXPECT_LT(md.find("`commit`"), shows);
  EXPECT_GT(md.find("`show diff`"), lifecycle);
}

TEST(Docs, EscapesMarkdownPipesInHelp) {
  const auto md = Generate();
  EXPECT_NE(md.find("Device \\| hostname"), std::string::npos);
}

TEST(Docs, DeterministicOutput) {
  EXPECT_EQ(Generate(), Generate());
}

}  // namespace
}  // namespace einheit::cli::docs
