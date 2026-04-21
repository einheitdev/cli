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
  // Psychotropic palette — hues lifted from karl's nvim colourscheme
  // (psychotropic.nvim). Louder and more playful than a typical
  // terminal theme, but each semantic stays distinct against a
  // near-black background.
  Theme t;
  t.good        = Rgb(0x99DA3D);  // lime — success, additions
  t.warn        = Rgb(0xF9CB52);  // khaki — caution, changes
  t.bad         = Rgb(0xFF5454);  // red — failure, removals
  t.dim         = Rgb(0x626262);  // grey39 — muted context
  t.emphasis    = Rgb(0xF6FAFA);  // near-white — headers
  t.info        = Rgb(0x4DB9F4);  // electric blue — labels
  t.border      = Rgb(0x4E4E4E);  // grey30 — table rules
  t.accent      = Rgb(0xE061F9);  // violet — logo + prompt glyph
  t.prompt_user = Rgb(0xADADF3);  // lavender — the `user` part
  t.prompt_at   = Rgb(0x808080);  // grey50 — the `@` separator
  t.prompt_host = Rgb(0x7FD3A5);  // turquoise — the host part
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
  t.prompt_user = ftxui::Color::MagentaLight;
  t.prompt_at   = ftxui::Color::GrayDark;
  t.prompt_host = ftxui::Color::CyanLight;
  return t;
}

auto DefaultLightTrueColor() -> Theme {
  // Darker foregrounds so each semantic carries enough contrast on
  // a near-white background. Same hues as the dark theme but lower
  // L* values.
  Theme t;
  t.good        = Rgb(0x3E7F2B);  // deep forest lime
  t.warn        = Rgb(0xB06B00);  // burnt amber
  t.bad         = Rgb(0xBF3C4F);  // wine coral
  t.dim         = Rgb(0x737B85);  // mid-slate
  t.emphasis    = Rgb(0x1C1D22);  // near-black, slight warm
  t.info        = Rgb(0x0077AA);  // ocean blue
  t.border      = Rgb(0xC3C7D0);  // pale steel
  t.accent      = Rgb(0x3351A9);  // royal blue
  t.prompt_user = Rgb(0x8839EF);  // royal purple — user
  t.prompt_at   = Rgb(0x9AA0A6);  // mid grey — @
  t.prompt_host = Rgb(0x005BAA);  // deep blue — host
  return t;
}

auto DefaultLightAnsi() -> Theme {
  Theme t;
  t.good        = ftxui::Color::Green;
  t.warn        = ftxui::Color::Yellow;
  t.bad         = ftxui::Color::Red;
  t.dim         = ftxui::Color::GrayLight;
  t.emphasis    = ftxui::Color::Black;
  t.info        = ftxui::Color::Blue;
  t.border      = ftxui::Color::GrayDark;
  t.accent      = ftxui::Color::Blue;
  t.prompt_user = ftxui::Color::Magenta;
  t.prompt_at   = ftxui::Color::GrayLight;
  t.prompt_host = ftxui::Color::Blue;
  return t;
}

auto DetectLightTerminal() -> bool {
  const char *env = std::getenv("COLORFGBG");
  if (!env) return false;
  std::string s(env);
  const auto last = s.find_last_of(';');
  if (last == std::string::npos) return false;
  try {
    const int bg = std::stoi(s.substr(last + 1));
    return bg == 7 || bg >= 10;
  } catch (...) {
    return false;
  }
}

auto PickTheme(const TerminalCaps &caps, bool prefer_light) -> Theme {
  const bool truecolor =
      !caps.force_plain &&
      (caps.colors == ColorDepth::TrueColor ||
       caps.colors == ColorDepth::Ansi256);
  if (prefer_light) {
    return truecolor ? DefaultLightTrueColor() : DefaultLightAnsi();
  }
  if (caps.force_plain || caps.colors == ColorDepth::None) {
    return DefaultDarkAnsi();
  }
  return truecolor ? DefaultDarkTrueColor() : DefaultDarkAnsi();
}

auto FgAnsi(ftxui::Color c) -> std::string {
  // FTXUI has Color::Print(background) returning an SGR. Wrap it in
  // ESC[..m brackets — Print returns only the payload digits.
  std::string payload = c.Print(/*is_background_color=*/false);
  if (payload.empty()) return "";
  return std::format("\x1b[{}m", payload);
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
    ReadColor(doc, "prompt_at", t.prompt_at);
    ReadColor(doc, "prompt_host", t.prompt_host);
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
