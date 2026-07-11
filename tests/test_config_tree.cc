/// @file test_config_tree.cc
/// @brief Tree rendering of flat config bodies.
// Copyright (c) 2026 Einheit Networks

#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/render/config_tree.h"

namespace einheit::cli::render {
namespace {

auto PlainCaps() -> TerminalCaps {
  TerminalCaps caps;
  caps.is_tty = false;
  caps.force_plain = true;
  caps.colors = ColorDepth::None;
  return caps;
}

auto RenderPlain(const std::string &body,
                 OutputFormat format = OutputFormat::Table)
    -> std::string {
  std::ostringstream out;
  Renderer r(out, PlainCaps(), format);
  RenderConfigTree(body, r);
  return out.str();
}

TEST(ConfigTree, FoldsDottedPathsIntoBlocks) {
  const auto out = RenderPlain(
      "ports.lan1.vlan.10=tagged\nhostname=sw1\n");
  EXPECT_NE(out.find("ports {"), std::string::npos);
  EXPECT_NE(out.find("lan1 {"), std::string::npos);
  EXPECT_NE(out.find("vlan {"), std::string::npos);
  EXPECT_NE(out.find("10: tagged"), std::string::npos);
  EXPECT_NE(out.find("hostname: sw1"), std::string::npos);
  // Two closing braces nest back out.
  EXPECT_NE(out.find("}"), std::string::npos);
}

TEST(ConfigTree, SortsSiblingsAlphabetically) {
  const auto out = RenderPlain("zebra=1\nalpha=2\n");
  EXPECT_LT(out.find("alpha: 2"), out.find("zebra: 1"));
}

TEST(ConfigTree, DiffMarkersLandInTheGutter) {
  const auto out = RenderPlain(
      "+dns.primary=9.9.9.9\n~hostname=b (was a)\n-ntp.server=x\n");
  EXPECT_NE(out.find("+   primary: 9.9.9.9"), std::string::npos);
  EXPECT_NE(out.find("~ hostname: b (was a)"), std::string::npos);
  EXPECT_NE(out.find("-   server: x"), std::string::npos);
}

TEST(ConfigTree, LooseLinesPassThrough) {
  const auto out = RenderPlain("(no configuration yet)\n");
  EXPECT_NE(out.find("(no configuration yet)"), std::string::npos);
}

TEST(ConfigTree, MachineFormatsGetTheFlatBody) {
  const std::string body = "ports.lan1.enabled=true\n";
  EXPECT_EQ(RenderPlain(body, OutputFormat::Json), body);
}

}  // namespace
}  // namespace einheit::cli::render
