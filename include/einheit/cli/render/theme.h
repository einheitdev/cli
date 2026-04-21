/// @file theme.h
/// @brief Semantic → colour mapping. Defaults work on dark
/// terminals; can be flipped for light terminals via COLORFGBG or
/// overridden via `~/.einheit/theme.yaml`.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_THEME_H_
#define INCLUDE_EINHEIT_CLI_RENDER_THEME_H_

#include <expected>
#include <string>

#include "einheit/cli/error.h"
#include "einheit/cli/render/table.h"

namespace einheit::cli::render {

/// A palette that maps Semantic values to ANSI colour names so the
/// renderer can use FTXUI Color::FromName(...) equivalents.
/// Concrete names come from ftxui::Color enum (e.g. "GreenLight").
struct Theme {
  std::string good = "GreenLight";
  std::string warn = "YellowLight";
  std::string bad = "RedLight";
  std::string dim = "GrayDark";
  std::string emphasis = "White";
  std::string info = "CyanLight";
  std::string border = "GrayLight";
  std::string accent = "CyanLight";   // logo + prompt glyph
};

/// Default palette for dark terminals.
/// @returns The dark-mode theme.
auto DefaultDarkTheme() -> Theme;

/// Default palette for light terminals (inverted dim/emphasis).
/// @returns The light-mode theme.
auto DefaultLightTheme() -> Theme;

/// Best-guess whether the terminal uses a light background. Reads
/// the COLORFGBG environment variable in the form "fg;bg" where `bg`
/// is a 0–15 ANSI palette index; 7, 15, and high values indicate a
/// light background.
/// @returns True when the terminal background appears light.
auto DetectLightTerminal() -> bool;

/// Errors raised by theme loading.
enum class ThemeError {
  /// File missing or unreadable.
  NotFound,
  /// YAML malformed.
  ParseFailed,
};

/// Load a theme file. Missing keys keep their default values.
/// @param path Absolute path to a YAML file with keys matching the
/// Theme struct members.
/// @param base Starting theme (overrides merge onto this).
/// @returns Merged Theme or ThemeError.
auto LoadTheme(const std::string &path, Theme base = DefaultDarkTheme())
    -> std::expected<Theme, Error<ThemeError>>;

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_THEME_H_
