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

/// Which optional verb families to register alongside the always-on
/// core. Split out from the core so a product that isn't ready for
/// candidate-config doesn't advertise `set`/`commit`/`configure` that
/// would then error confusingly (gap #6). Drive `config_verbs` off
/// real product capability — a schema plus a daemon/backend that can
/// hold and apply a candidate.
struct GlobalsOptions {
  /// Register the candidate-config lifecycle verbs (`configure`,
  /// `set`, `delete`, `commit`, `commit confirmed`, `confirm`,
  /// `rollback …`) and the config-introspection `show` verbs
  /// (`show config`, `show commits`, `show commit`, `show schema`).
  bool config_verbs = true;
};

/// Register the **non-optional** core utility verbs into `tree`:
/// `help`, `exit`, `quit`, `history`, `alias`, `watch`, `logs`,
/// `shell`, `show env`, `doctor`, `explain`, `theme …`, `statusbar`,
/// `macro …`, `daemon start`, `daemon status`. These never depend on a
/// candidate config, so `help`/`exit` can never be missing — the s5
/// footgun where forgetting one call dropped `help` entirely (gap #6).
/// @param tree Registry to populate.
/// @returns void on success or the first CommandTreeError raised.
auto RegisterCoreGlobals(CommandTree &tree)
    -> std::expected<void, Error<CommandTreeError>>;

/// Register the opt-in candidate-config verbs into `tree`. Call this
/// only for products that actually support a candidate configuration.
/// @param tree Registry to populate.
/// @returns void on success or the first CommandTreeError raised.
auto RegisterConfigGlobals(CommandTree &tree)
    -> std::expected<void, Error<CommandTreeError>>;

/// Register the framework-owned globals selected by `opts`: always the
/// core verbs, plus the config verbs when `opts.config_verbs` is set.
/// Idempotent per tree: the registry rejects duplicate paths, so
/// calling this twice on the same tree returns DuplicateRegistration.
/// @param tree Registry to populate.
/// @param opts Which optional verb families to include.
/// @returns void on success or the first CommandTreeError raised.
auto RegisterGlobals(CommandTree &tree, const GlobalsOptions &opts)
    -> std::expected<void, Error<CommandTreeError>>;

/// Back-compatible overload: registers core + config verbs (the
/// historical all-in behaviour). Equivalent to
/// `RegisterGlobals(tree, {})`.
/// @param tree Registry to populate.
/// @returns void on success or the first CommandTreeError raised.
auto RegisterGlobals(CommandTree &tree)
    -> std::expected<void, Error<CommandTreeError>>;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_GLOBALS_H_
