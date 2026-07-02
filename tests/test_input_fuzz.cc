/// @file test_input_fuzz.cc
/// @brief Fuzz the input surface — the command parser, schema
/// completion, and value completion — with malformed, oversized,
/// invalid-UTF-8, and control-character input. None may crash; each
/// call must return a value or a typed error. Run under ASan/UBSan this
/// also proves no out-of-bounds read walks off the end of a hostile
/// token.
// Copyright (c) 2026 Einheit Networks

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/schema.h"

namespace einheit::cli {
namespace {

auto Tokenize(const std::string &line) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string cur;
  for (const char c : line) {
    if (c == ' ' || c == '\t') {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

auto Tree() -> CommandTree {
  CommandTree tree;
  (void)RegisterGlobals(tree);
  return tree;
}

auto Schema() -> std::shared_ptr<const schema::Schema> {
  auto s = schema::LoadSchemaFromString(R"(
version: 1
product: fuzz
config:
  hostname:
    type: string
  port:
    type: integer
  mode:
    type: enum
    values: [fast, slow]
  interfaces:
    type: map
    key: string
    value:
      type: object
      fields:
        address:
          type: cidr
)");
  return s ? std::shared_ptr<const schema::Schema>(*s)
           : schema::DefaultSchema();
}

// Parsing arbitrary token vectors must never crash — worst case a typed
// UnknownCommand / MissingArgument error.
TEST(InputFuzz, ParseArbitraryLinesNeverCrashes) {
  const auto tree = Tree();
  const std::vector<std::string> lines = {
      "",
      "   ",
      "\t\t",
      "set",
      "set set set set set",
      "show \x01\x02\x03",
      std::string(100000, 'a'),                    // one huge token
      std::string(5000, ' '),                      // all whitespace
      "commit \xff\xfe\xfd invalid utf8",
      "\x1b[31mcolor injection\x1b[0m",
      "set hostname " + std::string(65536, 'x'),   // huge value
      "help help help",
      "配置 主机名 值",                             // multibyte utf8
      std::string(1, '\0') + "nulbyte",
  };
  for (const auto &line : lines) {
    auto tokens = Tokenize(line);
    // Must return; value or typed error, never a throw or crash.
    auto parsed = Parse(tree, tokens, RoleGate::AdminOnly);
    (void)parsed;
  }
  SUCCEED();
}

// Schema completion / validation over hostile partial paths must not
// walk off the end or crash.
TEST(InputFuzz, SchemaCompletionOnHostilePathsNeverCrashes) {
  auto schema = Schema();
  const auto tree = Tree();
  const std::vector<std::string> partials = {
      "",
      ".",
      "..",
      "...............",
      "interfaces.",
      "interfaces..",
      "interfaces.[0].",
      "interfaces.\xff\xff.",
      std::string(20000, '.'),
      std::string(20000, 'z'),
      "hostname.nonexistent.deep.path",
      "\x01\x02\x03",
      "mode.",
      "[999999999]",
      "interfaces.eth0.address.",
  };
  for (const auto &p : partials) {
    (void)schema::Completions(*schema, p);
    (void)schema::ValueCompletions(*schema, p, "");
    (void)schema::ValidatePath(*schema, p, "somevalue");
    (void)schema::HasPath(*schema, p);
    // The schema-aware command completion path (set <path>).
    (void)SuggestCompletions(tree, *schema, {"set"}, p);
    (void)SuggestCompletions(tree, *schema, {"set", p}, "");
  }
  SUCCEED();
}

// Random byte soup fed as command lines and completion partials — a
// blunt fuzz pass. Deterministic seed for reproducibility.
TEST(InputFuzz, RandomByteSoupNeverCrashes) {
  auto schema = Schema();
  const auto tree = Tree();
  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<int> len(0, 300);
  for (int i = 0; i < 500; ++i) {
    std::string s;
    const int n = len(rng);
    s.reserve(n);
    for (int j = 0; j < n; ++j) {
      s.push_back(static_cast<char>(byte(rng)));
    }
    auto tokens = Tokenize(s);
    (void)Parse(tree, tokens, RoleGate::AnyAuthenticated);
    (void)schema::Completions(*schema, s);
    (void)schema::ValidatePath(*schema, s, s);
    (void)SuggestCompletions(tree, *schema, {"set"}, s);
  }
  SUCCEED();
}

}  // namespace
}  // namespace einheit::cli
