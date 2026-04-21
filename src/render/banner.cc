/// @file banner.cc
/// @brief FTXUI-backed startup banner with ASCII fallback.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/banner.h"

#include <cctype>
#include <format>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

namespace einheit::cli::render {
namespace {

// Logo for the banner — figlet "standard" font, lowercase
// "einheit". ~32 cells wide × 5 rows. Compact enough to leave room
// for product info in a standard 80-col terminal.
constexpr const char *kLogo[] = {
    "       _       _          _ _   ",
    "  ___ (_)_ __ | |__   ___(_) |_ ",
    " / _ \\| | '_ \\| '_ \\ / _ \\ | __|",
    "|  __/| | | | | | | |  __/ | |_ ",
    " \\___||_|_| |_|_| |_|\\___|_|\\__|",
};

auto UnicodeBanner(const BannerInfo &info, std::uint16_t width)
    -> std::string {
  using namespace ftxui;

  // Logo column — cyan tinted figlet-standard lettering.
  constexpr int kLogoWidth = 33;
  std::vector<Element> logo_lines;
  for (const char *line : kLogo) {
    logo_lines.push_back(text(line) | color(Color::CyanLight));
  }

  // Info column — left-aligned text, fills remaining width. Names
  // render lowercase for consistency with the logo.
  auto to_lower = [](std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(c));
    return s;
  };
  std::vector<Element> info_lines;
  info_lines.push_back(text(to_lower(info.product_name)) | bold);
  info_lines.push_back(
      text(std::format("adapter: {}   version: {}",
                       info.adapter_name, info.version)) |
      color(Color::GrayDark));
  if (!info.target_name.empty()) {
    info_lines.push_back(
        text(std::format("target: {}", info.target_name)) |
        color(Color::Blue));
  }
  if (info.learning_mode) {
    info_lines.push_back(
        text("learning mode — state is in-memory") |
        color(Color::Yellow) | bold);
  }

  Element logo_col =
      vbox(std::move(logo_lines)) | size(WIDTH, EQUAL, kLogoWidth);
  Element info_col = vbox(std::move(info_lines)) | vcenter | flex;

  // One-line padding above and below the content so the logo has
  // room to breathe inside the border.
  Element body = vbox({
      text(""),
      hbox({text("  "), logo_col, text("    "), info_col,
            text("  ")}),
      text(""),
  });
  Element framed = body | border | color(Color::GrayLight);

  const int total_width =
      width > 0 ? static_cast<int>(width) : 80;
  auto screen = Screen::Create(
      Dimension::Fixed(total_width), Dimension::Fit(framed));
  Render(screen, framed);
  return screen.ToString() + "\n";
}

auto AsciiBanner(const BannerInfo &info) -> std::string {
  std::string out;
  out += "+---------------------------------------------+\n";
  out += std::format("| einheit  {:<34} |\n", info.product_name);
  out += std::format("| adapter: {:<14} version: {:<10} |\n",
                     info.adapter_name, info.version);
  if (!info.target_name.empty()) {
    out += std::format("| target: {:<35} |\n", info.target_name);
  }
  if (info.learning_mode) {
    out += "| LEARNING MODE — state is in-memory          |\n";
  }
  out += "+---------------------------------------------+\n";
  return out;
}

}  // namespace

auto Banner(const BannerInfo &info, const TerminalCaps &caps)
    -> std::string {
  if (caps.force_plain || !caps.unicode ||
      caps.colors == ColorDepth::None) {
    return AsciiBanner(info);
  }
  return UnicodeBanner(info, caps.width);
}

}  // namespace einheit::cli::render
