/// @file sparkline.h
/// @brief Compact time-series renderer. Maps a window of samples to
/// Unicode block characters (▁▂▃▄▅▆▇█), with an ASCII fallback that
/// encodes direction as `up/down/flat` instead of per-sample height.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_SPARKLINE_H_
#define INCLUDE_EINHEIT_CLI_RENDER_SPARKLINE_H_

#include <span>
#include <string>

#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {

/// Render a sparkline from `samples`. When the terminal can't draw
/// unicode, returns a short direction-hint string instead.
/// @param samples Non-empty window of numeric samples.
/// @param caps Terminal capabilities.
/// @returns Rendered string. Empty on empty input.
auto Sparkline(std::span<const double> samples,
               const TerminalCaps &caps) -> std::string;

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_SPARKLINE_H_
