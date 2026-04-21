/// @file workstation_state.h
/// @brief Per-user workstation state persisted between invocations.
///
/// Lives at `~/.einheit/state` (YAML). Currently tracks the active
/// target so `einheit use berlin` affects subsequent runs without
/// requiring `--target berlin` each time.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_WORKSTATION_STATE_H_
#define INCLUDE_EINHEIT_CLI_WORKSTATION_STATE_H_

#include <expected>
#include <optional>
#include <string>

#include "einheit/cli/error.h"

namespace einheit::cli::workstation {

/// Errors raised by workstation state I/O.
enum class StateError {
  /// Filesystem write/read failed.
  IoFailed,
  /// YAML document on disk was malformed.
  ParseFailed,
};

/// Persisted state. Keep this shape small — it grows slowly.
struct State {
  /// Target selected by the most recent `use` command.
  std::optional<std::string> active_target;
};

/// Default location of the state file.
/// @returns Absolute path `$HOME/.einheit/state`, or empty if
/// `$HOME` is unset.
auto DefaultPath() -> std::string;

/// Load state from an explicit path. Missing file is a success —
/// returned state is empty.
/// @param path Absolute path to the state file.
/// @returns Loaded (possibly-empty) State or StateError.
auto Load(const std::string &path)
    -> std::expected<State, Error<StateError>>;

/// Write state to disk, replacing any prior contents. Creates
/// parent directories.
/// @param path Absolute path to the state file.
/// @param s State to persist.
/// @returns void on success or StateError.
auto Save(const std::string &path, const State &s)
    -> std::expected<void, Error<StateError>>;

}  // namespace einheit::cli::workstation

#endif  // INCLUDE_EINHEIT_CLI_WORKSTATION_STATE_H_
