/// @file test_sparkline.cc
/// @brief Tests for the sparkline renderer.
// Copyright (c) 2026 Einheit Networks

#include <array>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/render/sparkline.h"
#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {

namespace {
auto UnicodeCaps() -> TerminalCaps {
  TerminalCaps c;
  c.unicode = true;
  return c;
}
auto AsciiCaps() -> TerminalCaps {
  TerminalCaps c;
  c.unicode = false;
  return c;
}
}  // namespace

TEST(Sparkline, EmptyReturnsEmpty) {
  EXPECT_EQ(Sparkline({}, UnicodeCaps()), "");
}

TEST(Sparkline, UnicodeProducesOneGlyphPerSample) {
  constexpr std::array<double, 4> s{1.0, 2.0, 3.0, 4.0};
  const auto line = Sparkline(s, UnicodeCaps());
  // Each block glyph is three UTF-8 bytes (U+258x range).
  EXPECT_EQ(line.size(), s.size() * 3u);
}

TEST(Sparkline, UnicodeMinSampleIsLowestBlock) {
  constexpr std::array<double, 4> s{1.0, 2.0, 3.0, 4.0};
  const auto line = Sparkline(s, UnicodeCaps());
  // First three bytes correspond to the lowest block U+2581.
  EXPECT_EQ(line.substr(0, 3), std::string("\u2581"));
}

TEST(Sparkline, UnicodeMaxSampleIsHighestBlock) {
  constexpr std::array<double, 4> s{1.0, 2.0, 3.0, 4.0};
  const auto line = Sparkline(s, UnicodeCaps());
  EXPECT_EQ(line.substr(line.size() - 3, 3), std::string("\u2588"));
}

TEST(Sparkline, UnicodeAllEqualStillRenders) {
  constexpr std::array<double, 3> s{5.0, 5.0, 5.0};
  const auto line = Sparkline(s, UnicodeCaps());
  EXPECT_EQ(line.size(), 9u);
  EXPECT_EQ(line.substr(0, 3), std::string("\u2581"));
}

TEST(Sparkline, AsciiUpTrend) {
  constexpr std::array<double, 3> s{1.0, 5.0, 10.0};
  EXPECT_EQ(Sparkline(s, AsciiCaps()), "up");
}

TEST(Sparkline, AsciiDownTrend) {
  constexpr std::array<double, 3> s{10.0, 5.0, 1.0};
  EXPECT_EQ(Sparkline(s, AsciiCaps()), "down");
}

TEST(Sparkline, AsciiFlatTrend) {
  constexpr std::array<double, 4> s{5.0, 5.0, 5.0, 5.0};
  EXPECT_EQ(Sparkline(s, AsciiCaps()), "flat");
}

}  // namespace einheit::cli::render
