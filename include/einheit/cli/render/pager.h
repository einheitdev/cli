/// @file pager.h
/// @brief Auto-pager for long render output.
///
/// Commands that can produce many rows (show config, show commits,
/// show schema on a big adapter) buffer their output into a
/// RenderBuffer. When flushed, if the buffer exceeds the terminal
/// height and stdout is a TTY, it's piped through `$PAGER` (default
/// `less -R`). Otherwise it streams straight to stdout.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_PAGER_H_
#define INCLUDE_EINHEIT_CLI_RENDER_PAGER_H_

#include <sstream>
#include <string>

#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {

/// Decide whether `content` should be paged based on caps + env.
/// @param content Rendered text that will hit stdout.
/// @param caps Terminal capabilities.
/// @returns True when the pager should run.
auto ShouldPage(const std::string &content, const TerminalCaps &caps)
    -> bool;

/// Write `content` either direct to stdout or via the user's pager.
/// @param content Pre-rendered text.
/// @param caps Terminal capabilities.
auto Flush(const std::string &content, const TerminalCaps &caps)
    -> void;

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_PAGER_H_
