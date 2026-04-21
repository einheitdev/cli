/// @file confirm.cc
/// @brief Yellow confirmation box.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/confirm.h"

#include <format>
#include <iostream>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

namespace einheit::cli::render {
namespace {

auto RenderBox(const std::string &message, std::ostream &out,
               const TerminalCaps &caps) -> void {
  using namespace ftxui;
  const bool colorful =
      !caps.force_plain && caps.colors != ColorDepth::None;

  if (!caps.unicode) {
    out << std::format("[!] {}\n", message);
    return;
  }

  auto body = vbox({
      hbox({text("warning  ") | bold, text(message)}),
      text(""),
      text("press y to continue, anything else to cancel"),
  });
  Element framed =
      body | borderRounded |
      (colorful ? color(Color::YellowLight) : nothing);
  auto screen = Screen::Create(Dimension::Fit(framed),
                               Dimension::Fit(framed));
  Render(screen, framed);
  out << screen.ToString() << '\n';
}

}  // namespace

auto ConfirmPrompt(const std::string &message, std::ostream &out,
                   std::istream &in, const TerminalCaps &caps)
    -> bool {
  RenderBox(message, out, caps);
  out << "[y/N] " << std::flush;
  std::string line;
  if (!std::getline(in, line)) return false;
  return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
}

}  // namespace einheit::cli::render
