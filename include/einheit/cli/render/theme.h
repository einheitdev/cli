/// @file theme.h
/// @brief Semantic → colour mapping for the whole rendering stack.
///
/// Colours are ftxui::Color values so truecolor terminals get the
/// real 24-bit RGB and older terminals get FTXUI's automatic
/// degradation to 256 / 16 / none. We always assume a dark
/// background; the curated RGB palette is the primary theme and
/// the ANSI-16 fallback is used only when caps can't do truecolor.
///
/// Per-user overrides live in `~/.einheit/theme.yaml`; colour values
/// accept either FTXUI enum names ("GreenLight") or `#RRGGBB` hex
/// literals.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_THEME_H_
#define INCLUDE_EINHEIT_CLI_RENDER_THEME_H_

#include <expected>
#include <string>

#include <ftxui/screen/color.hpp>

#include "einheit/cli/error.h"
#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {

/// Semantic palette. Every field maps one Semantic (or rendering
/// role) to a concrete ftxui::Color.
struct Theme {
  ftxui::Color good;
  ftxui::Color warn;
  ftxui::Color bad;
  ftxui::Color dim;
  ftxui::Color emphasis;
  ftxui::Color info;
  ftxui::Color border;
  ftxui::Color accent;       // logo + prompt glyph
  ftxui::Color prompt_user;  // user@target prefix
};

/// Rich 24-bit dark palette — primary theme on modern terminals.
/// @returns Truecolor dark theme.
auto DefaultDarkTrueColor() -> Theme;

/// ANSI-16 dark palette — used when caps don't report truecolor.
/// @returns ANSI dark theme.
auto DefaultDarkAnsi() -> Theme;

/// Pick a sensible theme based on caps.
/// @param caps Terminal capabilities.
/// @returns The theme that best matches the current terminal.
auto PickTheme(const TerminalCaps &caps) -> Theme;

/// Errors raised by theme loading.
enum class ThemeError {
  /// File missing or unreadable.
  NotFound,
  /// YAML malformed.
  ParseFailed,
};

/// Parse "GreenLight" / "green-light" / "#9CE66E" / "9CE66E"
/// (case-insensitive). On failure, returns the provided fallback.
/// @param name Colour name or hex literal.
/// @param fallback Value returned on parse failure.
/// @returns Parsed ftxui::Color or fallback.
auto ParseColor(const std::string &name, ftxui::Color fallback)
    -> ftxui::Color;

/// Load a theme file. Missing keys keep their values from `base`.
/// @param path Absolute path to the YAML file.
/// @param base Starting theme (overrides merge onto this).
/// @returns Merged Theme or ThemeError.
auto LoadTheme(const std::string &path, Theme base)
    -> std::expected<Theme, Error<ThemeError>>;

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_THEME_H_
