/// @file banner.h
/// @brief Startup banner rendering. Uses FTXUI for layout + colour
/// so the framework only owns one rendering backend, and degrades to
/// plain ASCII when the terminal can't handle unicode / ANSI.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_BANNER_H_
#define INCLUDE_EINHEIT_CLI_RENDER_BANNER_H_

#include <string>
#include <vector>

#include "einheit/cli/render/terminal_caps.h"
#include "einheit/cli/render/theme.h"

namespace einheit::cli::render {

/// Inputs to Banner(). Plain data so the shell can fill it from the
/// adapter's ProductMetadata and its own mode flags.
struct BannerInfo {
  /// Product display name, e.g. "Einheit Example Product".
  std::string product_name;
  /// Adapter version string.
  std::string version;
  /// Short adapter id ("example", "g-gateway") shown as a subtitle.
  std::string adapter_name;
  /// True when running against the in-process learning daemon.
  /// Renders a prominent yellow "LEARNING MODE" line.
  bool learning_mode = false;
  /// True when started under `--locked`. Renders a "[locked]" chip
  /// so operators see at a glance which capabilities are off.
  bool locked = false;
  /// Optional target name from `--target` / `einheit use`. Shown
  /// when non-empty.
  std::string target_name;
  /// Optional "tip of the day" line rendered dimmed under the info
  /// column. Empty means no tip.
  std::string tip;
};

/// Short hints rotated under the banner. Pure data so tests + other
/// callers can inspect or override the list.
/// @returns A curated list of one-line tips.
auto DefaultTips() -> std::vector<std::string>;

/// Pick a tip pseudo-randomly, seeded by the current time so each
/// run shows a different one but a single session is stable.
/// @returns One tip line from DefaultTips().
auto PickTip() -> std::string;

/// Render the banner to a string. Output includes a trailing
/// newline. Uses unicode box characters + ANSI colour when caps
/// allow; otherwise falls back to `+-|` borders and plain text.
/// Uses PickTheme(caps) for colours.
/// @param info Banner fields.
/// @param caps Terminal capabilities.
/// @returns Rendered banner ready for `std::cout`.
auto Banner(const BannerInfo &info, const TerminalCaps &caps)
    -> std::string;

/// Theme-aware overload.
/// @param info Banner fields.
/// @param caps Terminal capabilities.
/// @param theme Colour palette.
/// @returns Rendered banner.
auto Banner(const BannerInfo &info, const TerminalCaps &caps,
            const Theme &theme) -> std::string;

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_BANNER_H_
