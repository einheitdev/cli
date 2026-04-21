/// @file theme.cc
/// @brief Theme palette loading + light-background detection.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/theme.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace einheit::cli::render {
namespace {

auto MakeError(ThemeError code, std::string message)
    -> Error<ThemeError> {
  return Error<ThemeError>{code, std::move(message)};
}

auto ReadString(const YAML::Node &n, const std::string &key,
                std::string &out) -> void {
  if (n[key]) out = n[key].as<std::string>();
}

}  // namespace

auto DefaultDarkTheme() -> Theme { return {}; }

auto DefaultLightTheme() -> Theme {
  Theme t;
  t.good = "Green";
  t.warn = "Yellow";
  t.bad = "Red";
  t.dim = "GrayLight";    // flipped vs dark
  t.emphasis = "Black";   // flipped
  t.info = "Cyan";
  t.border = "GrayDark";  // flipped
  t.accent = "Cyan";
  return t;
}

auto DetectLightTerminal() -> bool {
  const char *env = std::getenv("COLORFGBG");
  if (!env) return false;
  // COLORFGBG is "fg;bg" or "fg;default;bg"; bg is a 0-15 index.
  std::string s(env);
  const auto last = s.find_last_of(';');
  if (last == std::string::npos) return false;
  const std::string bg = s.substr(last + 1);
  try {
    const int n = std::stoi(bg);
    // 7, 15, and values >= 10 generally indicate light backgrounds.
    return n == 7 || n >= 10;
  } catch (...) {
    return false;
  }
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
    ReadString(doc, "good", t.good);
    ReadString(doc, "warn", t.warn);
    ReadString(doc, "bad", t.bad);
    ReadString(doc, "dim", t.dim);
    ReadString(doc, "emphasis", t.emphasis);
    ReadString(doc, "info", t.info);
    ReadString(doc, "border", t.border);
    ReadString(doc, "accent", t.accent);
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
