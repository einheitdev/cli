/// @file confirm.h
/// @brief Yellow-bordered "are you sure?" prompt. Used before
/// destructive verbs (rollback previous, delete, shell escape).
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_CONFIRM_H_
#define INCLUDE_EINHEIT_CLI_RENDER_CONFIRM_H_

#include <iosfwd>
#include <string>

#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {

/// Print a warning box and read a y/N response from `in`. Non-TTY
/// input streams return false without prompting.
/// @param message Plain-english warning (one line).
/// @param out Destination for the box + prompt.
/// @param in Source for the response character.
/// @param caps Terminal capabilities.
/// @returns True iff the user typed `y` or `Y`.
auto ConfirmPrompt(const std::string &message, std::ostream &out,
                   std::istream &in, const TerminalCaps &caps)
    -> bool;

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_CONFIRM_H_
