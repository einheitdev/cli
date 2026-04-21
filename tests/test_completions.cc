/// @file test_completions.cc
/// @brief Schema-aware completion tests.
// Copyright (c) 2026 Einheit Networks

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/schema.h"

namespace einheit::cli {
namespace {

class ScopedYaml {
 public:
  explicit ScopedYaml(const std::string &body) {
    path_ = std::filesystem::temp_directory_path() /
            ("einheit_comp_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter_++) + ".yaml");
    std::ofstream(path_) << body;
  }
  ~ScopedYaml() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  auto Path() const -> std::string { return path_.string(); }

 private:
  static inline int counter_ = 0;
  std::filesystem::path path_;
};

constexpr const char *kSchema = R"(
version: 1
product: test
config:
  hostname:
    type: string
  port:
    type: integer
  interfaces:
    type: map
    key: string
    value:
      type: object
      fields:
        address:
          type: cidr
        vlan:
          type: integer
)";

}  // namespace

TEST(Completions, VerbsFromTreeWhenNotSet) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  ScopedYaml f(kSchema);
  auto sch = schema::LoadSchema(f.Path());
  ASSERT_TRUE(sch.has_value());

  auto c = SuggestCompletions(tree, **sch, {"show"}, "c");
  EXPECT_NE(std::find(c.begin(), c.end(), "config"), c.end());
}

TEST(Completions, SetRoutesToSchemaRoot) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  ScopedYaml f(kSchema);
  auto sch = schema::LoadSchema(f.Path());
  ASSERT_TRUE(sch.has_value());

  auto c = SuggestCompletions(tree, **sch, {"set"}, "h");
  EXPECT_NE(std::find(c.begin(), c.end(), "hostname"), c.end());
  // Tree-level verbs should NOT leak into schema-mode completion.
  EXPECT_EQ(std::find(c.begin(), c.end(), "configure"), c.end());
}

TEST(Completions, SetNestedSchemaPath) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  ScopedYaml f(kSchema);
  auto sch = schema::LoadSchema(f.Path());
  ASSERT_TRUE(sch.has_value());

  auto c = SuggestCompletions(tree, **sch, {"set"},
                              "interfaces.eth0.");
  EXPECT_NE(std::find(c.begin(), c.end(), "address"), c.end());
  EXPECT_NE(std::find(c.begin(), c.end(), "vlan"), c.end());
}

TEST(Completions, DeleteAlsoUsesSchema) {
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());
  ScopedYaml f(kSchema);
  auto sch = schema::LoadSchema(f.Path());
  ASSERT_TRUE(sch.has_value());

  auto c = SuggestCompletions(tree, **sch, {"delete"}, "p");
  EXPECT_NE(std::find(c.begin(), c.end(), "port"), c.end());
}

}  // namespace einheit::cli
