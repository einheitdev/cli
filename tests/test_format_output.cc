/// @file test_format_output.cc
/// @brief RenderFormatted tests across Table/JSON/YAML/set formats.
// Copyright (c) 2026 Einheit Networks

#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/render/table.h"
#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {

namespace {

auto PlainCaps() -> TerminalCaps {
  TerminalCaps c;
  c.width = 200;
  c.force_plain = true;
  c.colors = ColorDepth::None;
  return c;
}

auto MakeTable() -> Table {
  Table t;
  AddColumn(t, "name", Align::Left, Priority::High);
  AddColumn(t, "state", Align::Left, Priority::High);
  AddColumn(t, "latency", Align::Right, Priority::Medium);
  AddRow(t, {Cell{"munich"}, Cell{"up"}, Cell{"4.2ms"}});
  AddRow(t, {Cell{"berlin"}, Cell{"degraded"}, Cell{"120ms"}});
  return t;
}

}  // namespace

TEST(RenderFormatted, JsonProducesArrayOfObjects) {
  const Table t = MakeTable();
  std::ostringstream oss;
  Renderer r(oss, PlainCaps(), OutputFormat::Json);
  RenderFormatted(t, r);
  const auto s = oss.str();
  EXPECT_NE(s.find("\"name\":\"munich\""), std::string::npos);
  EXPECT_NE(s.find("\"state\":\"degraded\""), std::string::npos);
  EXPECT_NE(s.find("\"latency\":\"4.2ms\""), std::string::npos);
  EXPECT_EQ(s.front(), '[');
}

TEST(RenderFormatted, JsonEscapesSpecialChars) {
  Table t;
  AddColumn(t, "k", Align::Left, Priority::High);
  AddRow(t, {Cell{"a\"b\nc"}});
  std::ostringstream oss;
  Renderer r(oss, PlainCaps(), OutputFormat::Json);
  RenderFormatted(t, r);
  EXPECT_NE(oss.str().find("a\\\"b\\nc"), std::string::npos);
}

TEST(RenderFormatted, YamlUsesListEntries) {
  const Table t = MakeTable();
  std::ostringstream oss;
  Renderer r(oss, PlainCaps(), OutputFormat::Yaml);
  RenderFormatted(t, r);
  const auto s = oss.str();
  EXPECT_NE(s.find("- name: munich"), std::string::npos);
  EXPECT_NE(s.find("  state: up"), std::string::npos);
  EXPECT_NE(s.find("- name: berlin"), std::string::npos);
}

TEST(RenderFormatted, SetEmitsJunosStyle) {
  const Table t = MakeTable();
  std::ostringstream oss;
  Renderer r(oss, PlainCaps(), OutputFormat::Set);
  RenderFormatted(t, r);
  const auto s = oss.str();
  EXPECT_NE(s.find("set munich state up"), std::string::npos);
  EXPECT_NE(s.find("set munich latency 4.2ms"), std::string::npos);
  EXPECT_NE(s.find("set berlin state degraded"), std::string::npos);
}

TEST(RenderFormatted, TableFormatMatchesClassicRender) {
  const Table t = MakeTable();
  std::ostringstream a, b;
  Renderer r(a, PlainCaps(), OutputFormat::Table);
  RenderFormatted(t, r);
  Render(t, b, PlainCaps());
  EXPECT_EQ(a.str(), b.str());
}

TEST(RenderFormatted, YamlEmptyTableHandled) {
  Table t;
  std::ostringstream oss;
  Renderer r(oss, PlainCaps(), OutputFormat::Yaml);
  RenderFormatted(t, r);
  EXPECT_EQ(oss.str(), "[]\n");
}

}  // namespace einheit::cli::render
