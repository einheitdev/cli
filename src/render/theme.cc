/// @file theme.cc
/// @brief Theme palette loading + selection.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/theme.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace einheit::cli::render {
namespace {

auto MakeError(ThemeError code, std::string message)
    -> Error<ThemeError> {
  return Error<ThemeError>{code, std::move(message)};
}

auto RGB(std::uint8_t r, std::uint8_t g, std::uint8_t b)
    -> ftxui::Color {
  return ftxui::Color::RGB(r, g, b);
}

auto Rgb(std::uint32_t hex) -> ftxui::Color {
  return RGB(static_cast<std::uint8_t>((hex >> 16) & 0xFF),
             static_cast<std::uint8_t>((hex >> 8) & 0xFF),
             static_cast<std::uint8_t>(hex & 0xFF));
}

auto Lower(std::string s) -> std::string {
  for (auto &c : s) c = static_cast<char>(std::tolower(c));
  return s;
}

auto NamedColor(const std::string &lower_name,
                ftxui::Color fallback) -> ftxui::Color {
  static const std::unordered_map<std::string, ftxui::Color> kMap = {
      {"default",      ftxui::Color::Default},
      {"black",        ftxui::Color::Black},
      {"red",          ftxui::Color::Red},
      {"green",        ftxui::Color::Green},
      {"yellow",       ftxui::Color::Yellow},
      {"blue",         ftxui::Color::Blue},
      {"magenta",      ftxui::Color::Magenta},
      {"cyan",         ftxui::Color::Cyan},
      {"white",        ftxui::Color::White},
      {"graydark",     ftxui::Color::GrayDark},
      {"graylight",    ftxui::Color::GrayLight},
      {"redlight",     ftxui::Color::RedLight},
      {"greenlight",   ftxui::Color::GreenLight},
      {"yellowlight",  ftxui::Color::YellowLight},
      {"bluelight",    ftxui::Color::BlueLight},
      {"magentalight", ftxui::Color::MagentaLight},
      {"cyanlight",    ftxui::Color::CyanLight},
  };
  auto it = kMap.find(lower_name);
  return it == kMap.end() ? fallback : it->second;
}

auto HexDigit(char c, int &out) -> bool {
  if (c >= '0' && c <= '9') { out = c - '0'; return true; }
  if (c >= 'a' && c <= 'f') { out = c - 'a' + 10; return true; }
  if (c >= 'A' && c <= 'F') { out = c - 'A' + 10; return true; }
  return false;
}

auto TryParseHex(const std::string &s, ftxui::Color &out) -> bool {
  std::string hex = s;
  if (!hex.empty() && hex.front() == '#') hex.erase(0, 1);
  if (hex.size() != 6) return false;
  int vals[6];
  for (int i = 0; i < 6; ++i) {
    if (!HexDigit(hex[i], vals[i])) return false;
  }
  out = RGB(static_cast<std::uint8_t>(vals[0] * 16 + vals[1]),
            static_cast<std::uint8_t>(vals[2] * 16 + vals[3]),
            static_cast<std::uint8_t>(vals[4] * 16 + vals[5]));
  return true;
}

auto ReadColor(const YAML::Node &n, const std::string &key,
               ftxui::Color &out) -> void {
  if (!n[key]) return;
  out = ParseColor(n[key].as<std::string>(), out);
}

}  // namespace

auto DefaultDarkTrueColor() -> Theme {
  // Curated dark palette tuned so the six semantics read cleanly
  // against a dark-grey / near-black background. Loosely
  // Tokyo-Night-ish but our own picks.
  Theme t;
  t.good        = Rgb(0x9ECE6A);  // vivid lime — success
  t.warn        = Rgb(0xE0AF68);  // soft amber — caution
  t.bad         = Rgb(0xF7768E);  // coral — failure
  t.dim         = Rgb(0x6B7280);  // slate grey — muted context
  t.emphasis    = Rgb(0xE4E7EF);  // near-white, slight blue cast
  t.info        = Rgb(0x7DCFFF);  // sky blue — labels
  t.border      = Rgb(0x3B4048);  // muted steel
  t.accent      = Rgb(0x7AA2F7);  // periwinkle — logo + prompt glyph
  t.prompt_user = Rgb(0x565F89);  // dimmer blue — user@host prefix
  return t;
}

auto DefaultDarkAnsi() -> Theme {
  Theme t;
  t.good        = ftxui::Color::GreenLight;
  t.warn        = ftxui::Color::YellowLight;
  t.bad         = ftxui::Color::RedLight;
  t.dim         = ftxui::Color::GrayDark;
  t.emphasis    = ftxui::Color::White;
  t.info        = ftxui::Color::CyanLight;
  t.border      = ftxui::Color::GrayLight;
  t.accent      = ftxui::Color::CyanLight;
  t.prompt_user = ftxui::Color::GrayDark;
  return t;
}

auto PickTheme(const TerminalCaps &caps) -> Theme {
  if (caps.force_plain || caps.colors == ColorDepth::None) {
    return DefaultDarkAnsi();  // ignored when caller suppresses
  }
  if (caps.colors == ColorDepth::TrueColor ||
      caps.colors == ColorDepth::Ansi256) {
    return DefaultDarkTrueColor();
  }
  return DefaultDarkAnsi();
}

auto ParseColor(const std::string &name, ftxui::Color fallback)
    -> ftxui::Color {
  if (name.empty()) return fallback;
  ftxui::Color parsed;
  if (TryParseHex(name, parsed)) return parsed;
  std::string l = Lower(name);
  l.erase(std::remove(l.begin(), l.end(), '-'), l.end());
  l.erase(std::remove(l.begin(), l.end(), '_'), l.end());
  return NamedColor(l, fallback);
}

auto LoadTheme(const std::string &path, Theme base)
    -> std::expected<Theme, Error<ThemeError>> {
  if (!std::filesystem::exists(path)) {
    return std::unexpected(MakeError(
        ThemeError::NotFound,
        std::format("theme file not found: {}", path)));
  }
  try {
    auto doc = YAML::LoadFile(path);
    Theme t = base;
    ReadColor(doc, "good", t.good);
    ReadColor(doc, "warn", t.warn);
    ReadColor(doc, "bad", t.bad);
    ReadColor(doc, "dim", t.dim);
    ReadColor(doc, "emphasis", t.emphasis);
    ReadColor(doc, "info", t.info);
    ReadColor(doc, "border", t.border);
    ReadColor(doc, "accent", t.accent);
    ReadColor(doc, "prompt_user", t.prompt_user);
    return t;
  } catch (const YAML::Exception &e) {
    return std::unexpected(
        MakeError(ThemeError::ParseFailed, e.what()));
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(ThemeError::ParseFailed, e.what()));
  }
}

}  // namespace einheit::cli::render
