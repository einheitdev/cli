/// @file audit.h
/// @brief Client-side audit helper. The authoritative log is written
/// by the daemon; the CLI only stamps caller identity onto requests
/// and flags locally-handled commands (history, alias, exit).
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_AUDIT_H_
#define INCLUDE_EINHEIT_CLI_AUDIT_H_

#include <string>
#include <vector>

#include "einheit/cli/auth.h"

namespace einheit::cli::audit {

/// One locally-observed command — used for the client's own history
/// file and for stamping Requests before they cross the wire.
struct LocalEvent {
  /// RFC 3339 timestamp with millisecond precision.
  std::string timestamp;
  /// Caller identity resolved at session start.
  auth::CallerIdentity caller;
  /// Command path (e.g. "show tunnels").
  std::string command;
  /// Positional args after the path.
  std::vector<std::string> args;
  /// True iff the command was handled entirely in the CLI (no wire
  /// round trip). Such commands still enter the per-user history.
  bool handled_locally = false;
};

/// Stamp a request-level identity block onto an outbound wire
/// message. The daemon verifies authoritatively; this is advisory.
/// @param caller Resolved identity.
/// @param user_out Field to populate.
/// @param role_out Field to populate.
auto StampIdentity(const auth::CallerIdentity &caller,
                   std::string &user_out, std::string &role_out)
    -> void;

}  // namespace einheit::cli::audit

#endif  // INCLUDE_EINHEIT_CLI_AUDIT_H_
