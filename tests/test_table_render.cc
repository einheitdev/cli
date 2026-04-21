/// @file test_table_render.cc
/// @brief Tests for the FTXUI-backed table renderer. Because FTXUI's
/// exact byte output depends on the library version, we assert on
/// structural properties — presence of headers, ANSI codes for
/// semantics, column-drop behaviour — rather than exact strings.
// Copyright (c) 2026 Einheit Networks

#include <cstdint>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/render/table.h"
#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {
namespace {

auto PlainCaps(std::uint16_t width) -> TerminalCaps {
  TerminalCaps c;
  c.width = width;
  c.force_plain = true;
  c.colors = ColorDepth::None;
  c.unicode = false;
  return c;
}

auto ColorCaps(std::uint16_t width) -> TerminalCaps {
  TerminalCaps c;
  c.width = width;
  c.force_plain = false;
  c.colors = ColorDepth::Ansi16;
  c.unicode = true;
  return c;
}

auto MakeTunnelsTable() -> Table {
  Table t;
  AddColumn(t, "Tunnel", Align::Left, Priority::High);
  AddColumn(t, "Peer", Align::Left, Priority::High);
  AddColumn(t, "State", Align::Center, Priority::High);
  AddColumn(t, "Latency", Align::Right, Priority::Medium);
  AddColumn(t, "RX/TX", Align::Right, Priority::Low);

  AddRow(t, {Cell{"munich"}, Cell{"fleet://2:47"},
             Cell{"up", Semantic::Good}, Cell{"4.2ms"},
             Cell{"1.2M/800K"}});
  AddRow(t, {Cell{"berlin"}, Cell{"fleet://3:12"},
             Cell{"degraded", Semantic::Warn}, Cell{"120ms"},
             Cell{"200/100"}});
  return t;
}

}  // namespace

TEST(TableRender, PlainWideIncludesAllHeaders) {
  const Table t = MakeTunnelsTable();
  std::ostringstream oss;
  Render(t, oss, PlainCaps(120));
  const auto s = oss.str();
  EXPECT_NE(s.find("Tunnel"), std::string::npos);
  EXPECT_NE(s.find("Peer"), std::string::npos);
  EXPECT_NE(s.find("State"), std::string::npos);
  EXPECT_NE(s.find("Latency"), std::string::npos);
  EXPECT_NE(s.find("RX/TX"), std::string::npos);
  EXPECT_NE(s.find("munich"), std::string::npos);
  EXPECT_NE(s.find("[OK]"), std::string::npos);
  EXPECT_NE(s.find("[WARN]"), std::string::npos);
}

TEST(TableRender, DropsLowestPriorityWhenNarrow) {
  const Table t = MakeTunnelsTable();
  std::ostringstream oss;
  Render(t, oss, PlainCaps(55));
  const auto s = oss.str();
  EXPECT_EQ(s.find("RX/TX"), std::string::npos);
  EXPECT_NE(s.find("Tunnel"), std::string::npos);
}

TEST(TableRender, DropsMediumWhenVeryNarrow) {
  const Table t = MakeTunnelsTable();
  std::ostringstream oss;
  Render(t, oss, PlainCaps(40));
  const auto s = oss.str();
  EXPECT_EQ(s.find("RX/TX"), std::string::npos);
  EXPECT_EQ(s.find("Latency"), std::string::npos);
  EXPECT_NE(s.find("Tunnel"), std::string::npos);
  EXPECT_NE(s.find("State"), std::string::npos);
}

TEST(TableRender, ColorEmitsAnsiForSemanticGood) {
  const Table t = MakeTunnelsTable();
  std::ostringstream oss;
  Render(t, oss, ColorCaps(120));
  const auto s = oss.str();
  // Some form of green + yellow escape must be present.
  EXPECT_TRUE(s.find("\x1b[32m") != std::string::npos ||
              s.find("\x1b[92m") != std::string::npos);
  EXPECT_TRUE(s.find("\x1b[33m") != std::string::npos ||
              s.find("\x1b[93m") != std::string::npos);
}

TEST(TableRender, EmptyTableRendersNothing) {
  Table t;
  std::ostringstream oss;
  Render(t, oss, PlainCaps(80));
  EXPECT_EQ(oss.str(), "");
}

TEST(TableRender, UnicodeBordersWhenCapable) {
  const Table t = MakeTunnelsTable();
  std::ostringstream oss;
  Render(t, oss, ColorCaps(200));
  const auto s = oss.str();
  // At least one unicode box-drawing character is present.
  EXPECT_TRUE(s.find("\xe2\x94") != std::string::npos)
      << "expected unicode box chars";
}

TEST(TableRender, NoUnicodeWhenPlain) {
  const Table t = MakeTunnelsTable();
  std::ostringstream oss;
  Render(t, oss, PlainCaps(200));
  const auto s = oss.str();
  // Plain-mode output must not contain unicode box-drawing chars.
  EXPECT_EQ(s.find("\xe2\x94"), std::string::npos);
  EXPECT_FALSE(s.empty());
}

}  // namespace einheit::cli::render
