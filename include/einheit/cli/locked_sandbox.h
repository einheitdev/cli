/// @file locked_sandbox.h
/// @brief Cli-side defense-in-depth for --locked mode. Installs a
/// seccomp-bpf filter that denies execve/execveat at the cli's
/// own ABI boundary, complementing whatever filter a parent
/// launcher (e.g. einheit-shell-launcher) already installed.
///
/// Filters stack: every filter installed by an ancestor is
/// inherited and AND'd against the new one. So a cli launched
/// outside a sandbox still gets a deny on exec when --locked is
/// set; a cli launched inside einheit-shell-launcher gets the
/// launcher's audit-set denials plus this no-exec denial.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_LOCKED_SANDBOX_H_
#define INCLUDE_EINHEIT_CLI_LOCKED_SANDBOX_H_

#include <expected>

#include "einheit/cli/error.h"

namespace einheit::cli {

/// Errors raised by the locked-mode sandbox install.
enum class LockedSandboxError {
  /// PR_SET_NO_NEW_PRIVS prctl failed. Typically means the binary
  /// was started in a context where the kernel forbids the prctl
  /// (rare; usually only inside very restrictive containers).
  NoNewPrivsFailed,
  /// SECCOMP_SET_MODE_FILTER syscall failed. errno is captured in
  /// the Error message.
  SeccompFailed,
};

/// Install the cli-side seccomp filter. Idempotent — calling
/// twice installs two filters (both the same) and the kernel
/// ANDs them, which costs a tiny bit of evaluation time but is
/// not incorrect. Callers should invoke once at startup right
/// after argv parsing in main when --locked is set.
///
/// The filter denies execve and execveat with EPERM so any
/// attempt by the cli (or anything it links) to spawn another
/// program surfaces as a normal errno rather than SIGSYS.
auto InstallLockedSeccomp()
    -> std::expected<void, Error<LockedSandboxError>>;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_LOCKED_SANDBOX_H_
