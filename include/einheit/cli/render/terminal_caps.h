/// @file terminal_caps.h
/// @brief Terminal capability detection. Probed once at CLI startup,
/// passed to every renderer.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_TERMINAL_CAPS_H_
#define INCLUDE_EINHEIT_CLI_RENDER_TERMINAL_CAPS_H_

#include <cstdint>

namespace einheit::cli::render {

/// Colour depth supported by the current terminal.
enum class ColorDepth {
  None,
  Ansi16,
  Ansi256,
  TrueColor,
};

/// What the renderer knows about the output destination.
struct TerminalCaps {
  /// Colour depth.
  ColorDepth colors = ColorDepth::None;
  /// True if the terminal can render Unicode box / block chars.
  bool unicode = false;
  /// True if stdout is a real TTY.
  bool is_tty = false;
  /// True if the user (or pipe context) forced plain output.
  bool force_plain = false;
  /// Terminal width in columns.
  std::uint16_t width = 80;
  /// Terminal height in rows.
  std::uint16_t height = 24;
};

/// Probe env, isatty(), ioctl(TIOCGWINSZ), locale.
/// @returns A populated TerminalCaps.
auto DetectTerminal() -> TerminalCaps;

/// Flags mirrored from the argv (--color=always|never|auto,
/// --ascii, --width=N). Overrides applied on top of DetectTerminal().
struct CapOverrides {
  /// -1 = auto, 0 = never, 1 = always.
  int color = -1;
  /// Force ASCII-only box drawing regardless of unicode support.
  bool force_ascii = false;
  /// If non-zero, override detected width.
  std::uint16_t width = 0;
};

/// Apply the overrides to a base TerminalCaps.
/// @param base Detected capabilities.
/// @param overrides Values from argv.
/// @returns Effective TerminalCaps after overrides.
auto ApplyOverrides(TerminalCaps base, const CapOverrides &overrides)
    -> TerminalCaps;

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_TERMINAL_CAPS_H_
