/// @file test_schema.cc
/// @brief Tests for the schema YAML parser, validator, and
/// completion walker.
// Copyright (c) 2026 Einheit Networks

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/schema.h"

namespace einheit::cli::schema {
namespace {

class ScopedYaml {
 public:
  explicit ScopedYaml(const std::string &body) {
    path_ = std::filesystem::temp_directory_path() /
            ("einheit_schema_" + std::to_string(::getpid()) +
             "_" + std::to_string(counter_++) + ".yaml");
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

constexpr const char *kExample = R"(
version: 1
product: example

config:
  hostname:
    type: string
    required: true
    help: "Appliance hostname"

  port:
    type: integer
    range: [1, 65535]

  mode:
    type: enum
    values: [active, standby, off]

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
          range: [1, 4094]

  routes:
    type: list
    item:
      type: object
      fields:
        dest:
          type: cidr
          required: true
        metric:
          type: integer
)";

}  // namespace

TEST(Schema, LoadsTopLevel) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value()) << s.error().message;
  EXPECT_EQ((*s)->version, 1u);
  EXPECT_EQ((*s)->product, "example");
  EXPECT_GT((*s)->root.fields.size(), 0u);
}

TEST(Schema, ValidatesIntegerRange) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(ValidatePath(**s, "port", "443").has_value());
  EXPECT_FALSE(ValidatePath(**s, "port", "99999").has_value());
  EXPECT_FALSE(ValidatePath(**s, "port", "not_a_number").has_value());
}

TEST(Schema, ValidatesEnum) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(ValidatePath(**s, "mode", "active").has_value());
  EXPECT_FALSE(ValidatePath(**s, "mode", "bogus").has_value());
}

TEST(Schema, ValidatesCidr) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  // MapSpec values resolve through the key wildcard; "interfaces.eth0"
  // walks into the value object, then .address lands on the cidr.
  EXPECT_TRUE(ValidatePath(**s, "interfaces.eth0.address",
                           "10.0.0.1/24")
                  .has_value());
  EXPECT_FALSE(
      ValidatePath(**s, "interfaces.eth0.address", "not a cidr")
          .has_value());
}

TEST(Schema, CompletionsAtRoot) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  auto c = Completions(**s, "i");
  EXPECT_NE(std::find(c.begin(), c.end(), "interfaces"), c.end());
}

TEST(Schema, CompletionsNested) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  auto c = Completions(**s, "interfaces.eth0.");
  EXPECT_NE(std::find(c.begin(), c.end(), "address"), c.end());
  EXPECT_NE(std::find(c.begin(), c.end(), "vlan"), c.end());
}

TEST(Schema, UnknownPathRejected) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  EXPECT_FALSE(ValidatePath(**s, "not_a_field", "x").has_value());
}

TEST(Schema, TypoSuggestsDidYouMean) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  auto r = ValidatePath(**s, "hostanme", "x");
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("hostname"), std::string::npos);
}

TEST(Schema, FormatSchemaListsAllLeafs) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  const auto body = FormatSchema(**s);
  EXPECT_NE(body.find("hostname"), std::string::npos);
  EXPECT_NE(body.find("port"), std::string::npos);
  EXPECT_NE(body.find("mode"), std::string::npos);
  EXPECT_NE(body.find("interfaces.<name>.address"),
            std::string::npos);
  EXPECT_NE(body.find("interfaces.<name>.vlan"),
            std::string::npos);
}

TEST(Schema, FormatSchemaShowsTypes) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  const auto body = FormatSchema(**s);
  EXPECT_NE(body.find("integer [1..65535]"), std::string::npos);
  EXPECT_NE(body.find("enum {active, standby, off}"),
            std::string::npos);
  EXPECT_NE(body.find("cidr"), std::string::npos);
}

TEST(Schema, FormatSchemaRespectsPrefix) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  const auto body = FormatSchema(**s, "interfaces");
  EXPECT_NE(body.find("interfaces"), std::string::npos);
  EXPECT_EQ(body.find("hostname"), std::string::npos);
}

TEST(Schema, FormatSchemaEmptyPrefixGivesFullList) {
  ScopedYaml f(kExample);
  auto s = LoadSchema(f.Path());
  ASSERT_TRUE(s.has_value());
  const auto body = FormatSchema(**s, "not_a_real_prefix");
  EXPECT_NE(body.find("no paths"), std::string::npos);
}

}  // namespace einheit::cli::schema
