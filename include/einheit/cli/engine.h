/// @file engine.h
/// @brief Reusable command-execution engine.
///
/// The command-execution core, pulled out of the interactive shell so
/// it can be driven programmatically. Three front-ends share it:
/// the shell drives it (interactive), the UI will drive it later
/// (a button → a command), and confd drives it (apply/rollback →
/// commands). The engine takes a parsed command + caller identity/role,
/// sends it over a Transport, threads the candidate-config session
/// across configure → set → commit / rollback, emits an audit record,
/// and returns a structured result. Rendering, history, and
/// framework-local verbs stay in the front-end.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_ENGINE_H_
#define INCLUDE_EINHEIT_CLI_ENGINE_H_

#include <chrono>
#include <expected>
#include <optional>
#include <string>

#include "einheit/cli/audit.h"
#include "einheit/cli/auth.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/error.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/session.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::cli::engine {

/// Errors raised before a command reaches the wire.
enum class EngineError {
  /// No transport is attached to the context.
  TransportUnavailable,
  /// Command declares requires_session but no configure session is
  /// open — enforced here so every front-end gates identically.
  SessionRequired,
  /// Command is framework-local (empty wire_command); the engine only
  /// executes wire commands, front-ends handle local verbs.
  NotAWireCommand,
};

/// How the wire round-trip resolved.
enum class WireStatus {
  /// A response came back (its status may still be daemon-level Error).
  Ok,
  /// Transport timed out waiting for the reply.
  Timeout,
  /// Transport failed for a non-timeout reason (send/recv/codec).
  Failed,
};

/// Result of executing one wire command.
struct ExecOutcome {
  /// Daemon response. Present iff wire == WireStatus::Ok.
  std::optional<protocol::Response> response;
  /// Measured wire round-trip time.
  std::chrono::milliseconds rtt{0};
  /// Wire resolution.
  WireStatus wire = WireStatus::Ok;
  /// Populated when wire != Ok — the transport error message.
  std::string error_message;
};

/// Borrowed context the engine executes against. The caller owns the
/// transport and session; the engine reads identity and mutates the
/// session across lifecycle verbs.
struct Context {
  /// Connected transport to the daemon / embedded runtime.
  transport::Transport *tx = nullptr;
  /// Candidate-config session state, threaded across commands.
  Session *session = nullptr;
  /// Resolved caller identity, stamped onto each request.
  auth::CallerIdentity caller;
  /// Optional audit sink; one Record is emitted per Execute. May be
  /// empty (no-op).
  audit::Sink audit;
  /// Wire timeout for a single request.
  std::chrono::milliseconds timeout{std::chrono::seconds(30)};
};

/// Generate a correlation id for a Request. Not a true UUID — the
/// daemon only echoes it back for matching, so a random hex blob is
/// enough for uniqueness within a session.
/// @returns A 32-hex-char id string.
auto NewRequestId() -> std::string;

/// Build the wire Request for a parsed command in this context. Public
/// so front-ends and confd can inspect / reuse the envelope shape.
/// @param ctx Execution context (identity + session).
/// @param parsed Parsed command to encode.
/// @returns A fully-populated Request ready to send.
auto BuildRequest(const Context &ctx, const ParsedCommand &parsed)
    -> protocol::Request;

/// Execute one parsed wire command: enforce the session requirement,
/// send the request, thread session state on lifecycle verbs, and emit
/// an audit record. Framework-local verbs (empty wire_command) are
/// rejected with EngineError::NotAWireCommand — front-ends handle those.
/// @param ctx Borrowed execution context; session may be mutated.
/// @param parsed Parsed command to execute.
/// @returns ExecOutcome (even when the daemon returns an error status),
///   or EngineError for pre-wire rejections.
auto Execute(Context &ctx, const ParsedCommand &parsed)
    -> std::expected<ExecOutcome, Error<EngineError>>;

}  // namespace einheit::cli::engine

#endif  // INCLUDE_EINHEIT_CLI_ENGINE_H_
