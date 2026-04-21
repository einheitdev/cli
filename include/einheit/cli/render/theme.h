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

/// Rich 24-bit dark palette — primary theme on dark terminals.
/// @returns Truecolor dark theme.
auto DefaultDarkTrueColor() -> Theme;

/// ANSI-16 dark palette — used when caps don't report truecolor.
/// @returns ANSI dark theme.
auto DefaultDarkAnsi() -> Theme;

/// Rich 24-bit light palette — mirror of the dark palette with
/// darker foregrounds chosen to read on a white / cream background.
/// @returns Truecolor light theme.
auto DefaultLightTrueColor() -> Theme;

/// ANSI-16 light palette — non-bright variants so colours carry
/// enough contrast on a light terminal.
/// @returns ANSI light theme.
auto DefaultLightAnsi() -> Theme;

/// Best-effort detection: does the terminal have a light background?
/// Reads COLORFGBG (e.g. "0;15") — values of 7, 15, or >= 10 in the
/// background slot indicate a light terminal.
/// @returns True when the terminal likely uses a light background.
auto DetectLightTerminal() -> bool;

/// Pick a sensible theme based on caps + environment. When
/// `prefer_light` is true, picks a light palette; otherwise dark.
/// @param caps Terminal capabilities.
/// @param prefer_light Force the light variant.
/// @returns The theme that best matches the current terminal.
auto PickTheme(const TerminalCaps &caps, bool prefer_light = false)
    -> Theme;

/// Produce a raw ANSI SGR sequence for the given foreground colour,
/// suitable for use in prompts and status lines drawn without the
/// FTXUI screen layer. Returns "" when the colour is Default.
/// @param c Foreground colour.
/// @returns ANSI escape, e.g. "\x1b[38;2;122;162;247m".
auto FgAnsi(ftxui::Color c) -> std::string;

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
