/// @file auth.h
/// @brief Client-side auth context — who is this CLI session.
///
/// Stamped on every outgoing Request. Daemon re-verifies via
/// SO_PEERCRED (local) or CurveZMQ key (remote); these fields are
/// advisory.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_AUTH_H_
#define INCLUDE_EINHEIT_CLI_AUTH_H_

#include <expected>
#include <string>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/error.h"

namespace einheit::cli::auth {

/// Errors raised by auth resolution.
enum class AuthError {
  /// Current UID does not map to a known einheit user.
  UnknownUser,
  /// User has no role assignment.
  NoRole,
  /// Auth resolution failed for transport-specific reason.
  ResolutionFailed,
};

/// Resolved caller identity for the current session.
struct CallerIdentity {
  /// Einheit user name.
  std::string user;
  /// Resolved role at session start.
  RoleGate role = RoleGate::AnyAuthenticated;
  /// Transport the session came in on (ssh/console/tcp).
  std::string transport;
  /// Optional source address (remote IP for tcp transport).
  std::string source_addr;
};

/// Resolve the caller identity for a local-socket CLI session.
/// On local transport, pulls UID via getuid() and maps to einheit
/// user through config. Intended for the on-appliance case.
/// @returns CallerIdentity or AuthError.
auto ResolveLocal() -> std::expected<CallerIdentity, Error<AuthError>>;

}  // namespace einheit::cli::auth

#endif  // INCLUDE_EINHEIT_CLI_AUTH_H_
