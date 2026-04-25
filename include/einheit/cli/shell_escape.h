/// @file shell_escape.h
/// @brief Admin-only POSIX shell escape, audit-stamped on entry and
/// exit. The daemon is notified at both boundaries so the audit log
/// brackets whatever the operator ran.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_SHELL_ESCAPE_H_
#define INCLUDE_EINHEIT_CLI_SHELL_ESCAPE_H_

#include <expected>
#include <string>

#include "einheit/cli/auth.h"
#include "einheit/cli/error.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::cli::shell_escape {

/// Errors surfaced by the shell-escape helper.
enum class EscapeError {
  /// Caller role is not admin.
  NotAuthorised,
  /// Audit notification to the daemon failed.
  AuditFailed,
  /// `fork` / `exec` failed.
  ExecFailed,
};

/// Entry + exit hooks for tests that want to verify the daemon is
/// notified without actually spawning a shell. Real callers leave
/// this as the default (null) and let Escape() fork/exec.
struct Hooks {
  /// When non-null, called in place of the shell subprocess.
  /// Receives the resolved bash path string. Return value becomes
  /// the exit code of the subprocess.
  int (*run_shell)(const std::string &bash_path) = nullptr;
};

/// Drop to a POSIX shell (defaults to $SHELL, then /bin/bash). The
/// daemon is notified over the transport via `shell_enter` and
/// `shell_exit` wire messages bracketing the subprocess.
/// @param tx Connected transport; used to notify the daemon.
/// @param caller Resolved identity of the user running the command.
/// @param locked When true, refuse outright — `--locked` mode null-
/// routes every escape vector regardless of caller role. The daemon
/// is not notified because no shell is spawned.
/// @param hooks Optional override (tests).
/// @returns The shell's exit code on success, EscapeError otherwise.
auto Escape(transport::Transport &tx,
            const auth::CallerIdentity &caller, bool locked = false,
            const Hooks &hooks = {})
    -> std::expected<int, Error<EscapeError>>;

}  // namespace einheit::cli::shell_escape

#endif  // INCLUDE_EINHEIT_CLI_SHELL_ESCAPE_H_
