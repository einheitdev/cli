/// @file audit.h
/// @brief Client-side audit helper. The authoritative log is written
/// by the daemon; the CLI only stamps caller identity onto requests
/// and flags locally-handled commands (history, alias, exit).
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_AUDIT_H_
#define INCLUDE_EINHEIT_CLI_AUDIT_H_

#include <functional>
#include <optional>
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

/// One audit record emitted by the command engine per executed
/// command. The client engine emits these as an advisory trail; a
/// daemon-side runtime keeps the authoritative log with the same shape.
struct Record {
  /// RFC 3339 timestamp with millisecond precision.
  std::string timestamp;
  /// Caller user name.
  std::string user;
  /// Caller role, stringified (admin / operator / readonly).
  std::string role;
  /// Command path (e.g. "commit").
  std::string command;
  /// Wire verb sent to the daemon; empty for framework-local verbs.
  std::string wire_command;
  /// Positional args after the path.
  std::vector<std::string> args;
  /// Candidate-config session id in force, if any.
  std::optional<std::string> session_id;
  /// True when the command succeeded (no rejection, no daemon error).
  bool ok = true;
  /// Short outcome note: "ok", an error code, or a rejection reason.
  std::string outcome;
};

/// Sink the engine writes each Record to. A std::function keeps the
/// contract data-oriented (no new virtual): the shell can drop these,
/// a daemon can append to its authoritative log, a test can collect
/// them for assertions.
using Sink = std::function<void(const Record &)>;

/// Current wall-clock time as an RFC 3339 timestamp (UTC) with
/// millisecond precision. Used to stamp audit records.
/// @returns Timestamp string like "2026-07-02T14:30:00.123Z".
auto NowTimestamp() -> std::string;

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
