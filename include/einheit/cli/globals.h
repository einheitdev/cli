/// @file globals.h
/// @brief Framework-owned global commands.
///
/// Every einheit CLI — regardless of product — exposes the same
/// verbs for config lifecycle (`configure`, `commit`, `rollback`),
/// introspection (`show config`, `show commits`), and shell control
/// (`exit`, `help`, `history`, `alias`, `watch`). This module
/// registers them into a CommandTree. Adapters contribute only
/// product-specific nouns on top.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_GLOBALS_H_
#define INCLUDE_EINHEIT_CLI_GLOBALS_H_

#include <expected>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/error.h"

namespace einheit::cli {

/// Register every framework-owned global into `tree`. Idempotent
/// per tree: the registry rejects duplicate paths, so calling this
/// twice on the same tree returns a DuplicateRegistration error.
/// @param tree Registry to populate.
/// @returns void on success or the first CommandTreeError raised.
auto RegisterGlobals(CommandTree &tree)
    -> std::expected<void, Error<CommandTreeError>>;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_GLOBALS_H_
