/// @file test_terminal_caps.cc
/// @brief Tests for ApplyOverrides; detection itself is environment-
/// sensitive and covered separately.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {

TEST(TerminalCaps, ForceColorNever) {
  TerminalCaps base;
  base.colors = ColorDepth::TrueColor;
  CapOverrides ov;
  ov.color = 0;
  auto out = ApplyOverrides(base, ov);
  EXPECT_EQ(out.colors, ColorDepth::None);
  EXPECT_TRUE(out.force_plain);
}

TEST(TerminalCaps, ForceColorAlways) {
  TerminalCaps base;
  base.colors = ColorDepth::None;
  base.force_plain = true;
  CapOverrides ov;
  ov.color = 1;
  auto out = ApplyOverrides(base, ov);
  EXPECT_FALSE(out.force_plain);
  EXPECT_NE(out.colors, ColorDepth::None);
}

TEST(TerminalCaps, OverrideWidth) {
  TerminalCaps base;
  base.width = 80;
  CapOverrides ov;
  ov.width = 120;
  auto out = ApplyOverrides(base, ov);
  EXPECT_EQ(out.width, 120);
}

TEST(TerminalCaps, ForceAscii) {
  TerminalCaps base;
  base.unicode = true;
  CapOverrides ov;
  ov.force_ascii = true;
  auto out = ApplyOverrides(base, ov);
  EXPECT_FALSE(out.unicode);
}

}  // namespace einheit::cli::render
