/// @file session.h
/// @brief Candidate-config session state held by the CLI. The daemon
/// owns the authoritative candidate; this struct is the client's
/// view of which session_id is active, whether we're in configure
/// mode, and what the last-seen candidate hash was.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_SESSION_H_
#define INCLUDE_EINHEIT_CLI_SESSION_H_

#include <cstdint>
#include <optional>
#include <string>

namespace einheit::cli {

/// Tracks the active candidate-config session, if any.
struct Session {
  /// True while the CLI is inside `configure` and before commit /
  /// rollback. Drives the prompt and allows `set` / `delete`.
  bool in_configure = false;
  /// Session id issued by the daemon on the `configure` reply.
  std::optional<std::string> session_id;
  /// Last candidate hash returned by the daemon; surfaced in the
  /// prompt or on `show diff` to detect drift.
  std::optional<std::string> candidate_hash;
  /// Set when the daemon has accepted a `commit confirmed N` and is
  /// counting down. The value is the epoch-seconds deadline after
  /// which the daemon auto-rolls back. Cleared on the next `commit`
  /// (acknowledge) or when the CLI reconnects and the daemon says
  /// the window already closed.
  std::optional<std::int64_t> confirm_deadline;
};

/// Reset to non-configure state. Called on commit, rollback
/// candidate, or transport disconnect.
/// @param s Session to clear.
auto ClearSession(Session &s) -> void;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_SESSION_H_
