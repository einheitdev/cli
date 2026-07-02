/// @file supervisor.h
/// @brief Thin crash supervisor for the interactive CLI.
///
/// The last line of crash containment: run the shell in a forked child
/// so that if it dies from a fault signal (a SEGV the prevention layers
/// missed), the parent reaps it and prints a clear notice — "the CLI
/// crashed running <X>, log at <path>" — instead of dumping the SSH
/// user into a dead prompt. A clean exit passes the child's status
/// through untouched; a graceful termination signal (TERM/INT) is
/// reported as such, not as a crash.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_SUPERVISOR_H_
#define INCLUDE_EINHEIT_CLI_SUPERVISOR_H_

#include <functional>
#include <ostream>
#include <string>

namespace einheit::cli {

/// How the supervised child ended.
enum class ChildOutcome {
  /// Child returned / exited normally (status carries the exit code).
  Exited,
  /// Child died from a fault signal (SEGV/ABRT/BUS/ILL/FPE) — a crash.
  Crashed,
  /// Child was killed by a termination signal (TERM/INT/QUIT/HUP) —
  /// a graceful stop, not a crash.
  Terminated,
};

/// Options controlling the supervisor's reporting.
struct SupervisorOptions {
  /// Path referenced in the crash notice, where the child's fault
  /// handler wrote the signal + backtrace. Informational only.
  std::string crash_log_path;
  /// Stream the crash / termination notice is written to. Defaults to
  /// std::cerr; overridable for tests.
  std::ostream *out = nullptr;
};

/// Structured result of a supervised run, exposed for tests. The
/// process exit code returned by RunSupervised is derived from this.
struct SupervisorResult {
  /// How the child ended.
  ChildOutcome outcome = ChildOutcome::Exited;
  /// Child's exit code (valid when outcome == Exited).
  int exit_code = 0;
  /// Signal number that killed the child (valid when Crashed /
  /// Terminated).
  int signal = 0;
  /// Last command the child recorded before dying (valid when Crashed).
  std::string last_command;
};

/// Run `child_main` under supervision. Forks; the child runs
/// `child_main` and exits with its return value; the parent reaps the
/// child and, on a fault-signal death, prints a crash notice naming the
/// last command and the log path. Fault signal handlers must already be
/// installed (they are inherited by the child) so the crash is also
/// logged with a backtrace.
/// @param child_main The shell entry point; its int return is the
///   child's exit code.
/// @param opts Reporting options.
/// @param result Optional out-param populated with the structured
///   outcome.
/// @returns The process exit code: the child's code on a clean exit,
///   or 128 + signal on a signal death.
auto RunSupervised(const std::function<int()> &child_main,
                   const SupervisorOptions &opts,
                   SupervisorResult *result = nullptr) -> int;

/// Classify a signal number as a fault (crash) signal versus a
/// graceful-termination signal. Exposed for tests and the supervisor.
/// @param sig Signal number.
/// @returns true iff `sig` is SEGV/ABRT/BUS/ILL/FPE.
auto IsFaultSignal(int sig) -> bool;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_SUPERVISOR_H_
