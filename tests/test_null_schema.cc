/// @file test_null_schema.cc
/// @brief Regression tests for gap #5 — the null-schema SIGSEGV.
///
/// The s5 crash: an adapter that never populated its schema returned a
/// `const Schema&` dereferenced from a null `shared_ptr`, and
/// `set i`<tab> routed through `SuggestCompletions(..., GetSchema(), …)`
/// which dereferenced it → SIGSEGV. `try/catch` cannot catch a
/// segfault, so the fix is structural: non-null by construction via
/// `SchemaHandle` / `DefaultSchema()`. These tests exercise that a
/// default-constructed schema surface completes cleanly instead of
/// crashing, plus the in-code `LoadSchemaFromString` builder.
// Copyright (c) 2026 Einheit Networks

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/schema.h"

namespace einheit::cli {
namespace {

using schema::DefaultSchema;
using schema::LoadSchemaFromString;
using schema::Schema;
using schema::SchemaHandle;

// A minimal adapter that never sets its schema — exactly the s5 shape
// that used to hold a null shared_ptr. With SchemaHandle the member is
// non-null by construction, so GetSchema() returns the empty default.
class NoSchemaAdapter : public ProductAdapter {
 public:
  auto Metadata() const -> ProductMetadata override { return {}; }
  auto GetSchema() const -> const Schema & override {
    return schema_.Get();
  }
  auto ControlSocketPath() const -> std::string override { return ""; }
  auto EventSocketPath() const -> std::string override { return ""; }
  auto Commands() const -> std::vector<CommandSpec> override {
    return {};
  }
  auto RenderResponse(const CommandSpec &, const protocol::Response &,
                      render::Renderer &) const -> void override {}
  auto EventTopicsFor(const CommandSpec &) const
      -> std::vector<std::string> override {
    return {};
  }
  auto RenderEvent(const std::string &, const protocol::Event &,
                   render::Renderer &) const -> void override {}

 private:
  SchemaHandle schema_;  // default → DefaultSchema(), never null
};

// The exact crash flow: `set i`<tab> over an adapter with no schema.
// Before the fix this dereferenced null and killed the process; now it
// must return an (empty) candidate list and survive.
TEST(NullSchema, SetCompletionOverEmptySchemaDoesNotCrash) {
  NoSchemaAdapter adapter;
  CommandTree tree;
  ASSERT_TRUE(RegisterGlobals(tree).has_value());

  const auto &schema = adapter.GetSchema();
  auto candidates = SuggestCompletions(tree, schema, {"set"}, "i");
  // No schema fields → no path candidates. The point is we got here.
  EXPECT_TRUE(candidates.empty());
}

// GetSchema() must yield a usable, empty-but-valid schema.
TEST(NullSchema, EmptyAdapterSchemaIsValidAndEmpty) {
  NoSchemaAdapter adapter;
  const auto &schema = adapter.GetSchema();
  EXPECT_EQ(schema.version, 1u);
  EXPECT_TRUE(schema.product.empty());
  EXPECT_TRUE(schema.root.fields.empty());
}

// A default handle and a handle built from null both resolve to the
// same shared default instance.
TEST(NullSchema, HandleNullFallsBackToDefault) {
  SchemaHandle def;
  SchemaHandle from_null_const{std::shared_ptr<const Schema>()};
  SchemaHandle from_null_mut{std::shared_ptr<Schema>()};
  EXPECT_EQ(&def.Get(), DefaultSchema().get());
  EXPECT_EQ(&from_null_const.Get(), DefaultSchema().get());
  EXPECT_EQ(&from_null_mut.Get(), DefaultSchema().get());
}

// A handle built from a real schema exposes that schema, not the
// default.
TEST(NullSchema, HandleWrapsRealSchema) {
  auto loaded = LoadSchemaFromString(R"(
version: 1
product: widget
config:
  hostname:
    type: string
)");
  ASSERT_TRUE(loaded.has_value());
  SchemaHandle h{*loaded};
  EXPECT_EQ(h.Get().product, "widget");
  EXPECT_NE(&h.Get(), DefaultSchema().get());
}

// Completion / validation over the default schema are well-defined
// no-ops, never a deref of null.
TEST(NullSchema, OperationsOverDefaultSchemaAreSafe) {
  const auto &schema = *DefaultSchema();
  EXPECT_TRUE(schema::Completions(schema, "anything").empty());
  EXPECT_TRUE(
      schema::ValueCompletions(schema, "a.b", "x").empty());
  auto v = schema::ValidatePath(schema, "no.such.path", "1");
  EXPECT_FALSE(v.has_value());  // unknown path, but no crash
}

// In-code building: LoadSchemaFromString parses the same document the
// old YAML-tempfile round-trip did, without touching the filesystem.
TEST(NullSchema, LoadSchemaFromStringParses) {
  auto s = LoadSchemaFromString(R"(
version: 2
product: inproc
config:
  port:
    type: integer
)");
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ((*s)->version, 2u);
  EXPECT_EQ((*s)->product, "inproc");
  auto c = schema::Completions(**s, "p");
  EXPECT_NE(std::find(c.begin(), c.end(), "port"), c.end());
}

// Malformed / incomplete embedded YAML is a catchable SchemaError, not
// a throw that escapes or a crash.
TEST(NullSchema, LoadSchemaFromStringRejectsGarbage) {
  auto missing = LoadSchemaFromString("just: a scalar\n");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, schema::SchemaError::MissingField);

  auto broken = LoadSchemaFromString("::: not : valid : yaml :::\n");
  EXPECT_FALSE(broken.has_value());
}

}  // namespace
}  // namespace einheit::cli
