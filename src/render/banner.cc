/// @file banner.cc
/// @brief FTXUI-backed startup banner with ASCII fallback.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/banner.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <format>
#include <random>
#include <sstream>
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

// Per-deployment branding override.  Adapters that ship under their
// own product name (e.g. hyper-derp's `hd-cli`) set EINHEIT_BRAND in
// the environment to replace the literal word "einheit" in the
// plain-ASCII banner, and EINHEIT_LOGO_PATH to a file whose
// contents are used as the logo column instead of `kLogo`.  The
// file may contain ANSI escapes (chafa output is the canonical
// case); UnicodeBanner passes them through to the terminal
// untouched and lets FTXUI handle the layout.
auto BrandOverride() -> std::string {
  if (const char *env = std::getenv("EINHEIT_BRAND")) {
    if (env[0] != '\0') return env;
  }
  return "einheit";
}

auto LogoOverride() -> std::vector<std::string> {
  std::vector<std::string> out;
  const char *path = std::getenv("EINHEIT_LOGO_PATH");
  if (path == nullptr || path[0] == '\0') return out;
  std::ifstream f(path);
  if (!f) return out;
  std::string line;
  while (std::getline(f, line)) {
    out.push_back(std::move(line));
  }
  return out;
}

// Renders the override banner: raw logo contents (typically chafa
// output with ANSI escapes) followed by a plain-text product info
// block. Bypasses FTXUI because FTXUI's `text()` element treats
// the ANSI escapes as opaque characters and breaks layout.
auto OverrideBanner(const BannerInfo &info,
                    const std::vector<std::string> &logo)
    -> std::string {
  std::string out;
  out += '\n';
  for (const auto &line : logo) {
    out += line;
    out += '\n';
  }
  out += std::format("  {}\n", info.product_name);
  out += std::format("  adapter: {}   version: {}\n",
                     info.adapter_name, info.version);
  if (!info.target_name.empty()) {
    out += std::format("  target: {}\n", info.target_name);
  }
  if (info.learning_mode) {
    out += "  learning mode — state is in-memory\n";
  }
  if (info.locked) {
    out += "  [locked] — shell escape, pager spawn, file paths off\n";
  }
  if (!info.tip.empty()) {
    out += std::format("  tip: {}\n", info.tip);
  }
  out += '\n';
  return out;
}

auto UnicodeBanner(const BannerInfo &info, std::uint16_t width,
                   const Theme &theme) -> std::string {
  using namespace ftxui;

  constexpr int kLogoWidth = 33;
  std::vector<Element> logo_lines;
  for (const char *line : kLogo) {
    logo_lines.push_back(text(line) | color(theme.accent));
  }

  auto to_lower = [](std::string s) {
    for (auto &c : s) c = static_cast<char>(std::tolower(c));
    return s;
  };
  std::vector<Element> info_lines;
  info_lines.push_back(text(to_lower(info.product_name)) | bold |
                       color(theme.emphasis));
  info_lines.push_back(
      text(std::format("adapter: {}   version: {}",
                       info.adapter_name, info.version)) |
      color(theme.dim));
  if (!info.target_name.empty()) {
    info_lines.push_back(
        text(std::format("target: {}", info.target_name)) |
        color(theme.info));
  }
  if (info.learning_mode) {
    info_lines.push_back(
        text("learning mode — state is in-memory") |
        color(theme.warn) | bold);
  }
  if (info.locked) {
    info_lines.push_back(
        text("[locked] — shell escape, pager spawn, file paths off") |
        color(theme.bad) | bold);
  }
  if (!info.tip.empty()) {
    info_lines.push_back(
        text(std::format("tip: {}", info.tip)) |
        color(theme.dim));
  }

  Element logo_col =
      vbox(std::move(logo_lines)) | size(WIDTH, EQUAL, kLogoWidth);
  Element info_col = vbox(std::move(info_lines)) | vcenter | flex;

  Element body = vbox({
      text(""),
      hbox({text("  "), logo_col, text("    "), info_col,
            text("  ")}),
      text(""),
  });
  Element framed = body | border | color(theme.border);

  const int total_width =
      width > 0 ? static_cast<int>(width) : 80;
  auto screen = Screen::Create(Dimension::Fixed(total_width),
                               Dimension::Fit(framed));
  Render(screen, framed);
  return screen.ToString() + "\n";
}

auto AsciiBanner(const BannerInfo &info) -> std::string {
  const auto brand = BrandOverride();
  // Inside-the-bars row width is 43 chars (the "+---...---+" runs
  // are 45 dashes; subtract two spaces for the leading/trailing
  // padding). Build the brand+product row dynamically so a long
  // brand name (e.g. "hyper-derp") doesn't overflow.
  constexpr std::size_t kRowWidth = 43;
  std::string row = brand + "  " + info.product_name;
  if (row.size() < kRowWidth) {
    row += std::string(kRowWidth - row.size(), ' ');
  } else if (row.size() > kRowWidth) {
    row.resize(kRowWidth);
  }
  std::string out;
  out += "+---------------------------------------------+\n";
  out += "| " + row + " |\n";
  out += std::format("| adapter: {:<14} version: {:<10} |\n",
                     info.adapter_name, info.version);
  if (!info.target_name.empty()) {
    out += std::format("| target: {:<35} |\n", info.target_name);
  }
  if (info.learning_mode) {
    out += "| LEARNING MODE — state is in-memory          |\n";
  }
  if (info.locked) {
    out += "| [LOCKED] — escape vectors disabled          |\n";
  }
  out += "+---------------------------------------------+\n";
  return out;
}

}  // namespace

auto Banner(const BannerInfo &info, const TerminalCaps &caps)
    -> std::string {
  return Banner(info, caps, PickTheme(caps));
}

auto Banner(const BannerInfo &info, const TerminalCaps &caps,
            const Theme &theme) -> std::string {
  // Adapter-supplied logo file (e.g. chafa output) takes precedence
  // when the terminal can render it.  Falls back to plain-ASCII for
  // dumb terminals because chafa output is meaningless without
  // colour.  Keeping this check here means EINHEIT_LOGO_PATH
  // honours --color=never and `TERM=dumb` like the rest of the
  // banner.
  if (!caps.force_plain && caps.unicode &&
      caps.colors != ColorDepth::None) {
    auto override_logo = LogoOverride();
    if (!override_logo.empty()) {
      return OverrideBanner(info, override_logo);
    }
  }
  if (caps.force_plain || !caps.unicode ||
      caps.colors == ColorDepth::None) {
    return AsciiBanner(info);
  }
  return UnicodeBanner(info, caps.width, theme);
}

auto DefaultTips() -> std::vector<std::string> {
  return {
      "press `?` mid-line for context help",
      "`show schema` lists every configurable path",
      "`show config interfaces` narrows a dump to a subtree",
      "`!!` reruns your last command",
      "`history` shows what you ran earlier this session",
      "tab completes verbs and schema paths",
      "YAML aliases at ~/.einheit/aliases.yaml — share with `include:`",
      "`commit confirmed 10` auto-rolls back after 10s if no follow-up",
      "`--format json` pipes cleanly into jq",
      "`exit` in configure drops back to operational",
  };
}

auto PickTip() -> std::string {
  auto tips = DefaultTips();
  if (tips.empty()) return {};
  const auto seed =
      std::chrono::system_clock::now().time_since_epoch().count();
  std::mt19937_64 rng(static_cast<std::uint64_t>(seed));
  std::uniform_int_distribution<std::size_t> pick(0, tips.size() - 1);
  return tips[pick(rng)];
}

}  // namespace einheit::cli::render
