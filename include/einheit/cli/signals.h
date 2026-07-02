/// @file signals.h
/// @brief Industrial signal regime for the einheit CLI.
///
/// Crash safety has a "contain" half: when prevention fails, a crash
/// must be *diagnosed* (last command + backtrace logged) and never
/// leave a dead session, and routine control signals must be handled
/// deliberately rather than killing the process by default. This module
/// implements that contract for the CLI process. See
/// `SIGNAL_HANDLING.md`.
///
/// Two mechanisms, by signal kind:
///  - **Synchronous faults** (SEGV/ABRT/BUS/ILL/FPE) — a `sigaction`
///    handler that is async-signal-safe: it `write()`s a diagnostic to a
///    pre-opened fd, then restores the default disposition and re-raises
///    so a core dump is still produced. These fire on the faulting
///    thread and cannot be deferred.
///  - **Asynchronous control signals** (TERM/INT/QUIT/HUP/USR1/USR2) —
///    blocked in the process and drained from a `signalfd` by a
///    dedicated thread, so they are handled in a normal (non-async)
///    context where allocating and logging are safe.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_SIGNALS_H_
#define INCLUDE_EINHEIT_CLI_SIGNALS_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace einheit::cli::signals {

/// Record the command line about to be dispatched, into a fixed
/// async-signal-safe buffer that the fault handler reads. Call it just
/// before running each command so that if a handler crashes, the crash
/// log names the exact input that triggered it (the s5 `set i<tab>`
/// diagnosis). Truncated to an internal bound; never allocates.
/// @param command The command line being executed.
auto SetLastCommand(std::string_view command) -> void;

/// The most recent value passed to SetLastCommand (for tests / status
/// dumps). Returns an empty string if none was set.
auto LastCommand() -> std::string;

/// Additionally mirror every SetLastCommand write into `buf` (NUL-
/// terminated, capped at `cap`). Used by the supervisor: it points this
/// at a shared-memory page before forking the shell, so after the child
/// dies the parent can read the last command the child was running and
/// name it in the crash notice. Pass nullptr to stop mirroring.
/// @param buf Destination buffer (e.g. shared memory). May be nullptr.
/// @param cap Size of `buf` in bytes.
auto MirrorLastCommandTo(char *buf, std::size_t cap) -> void;

/// Install synchronous fault handlers (SEGV/ABRT/BUS/ILL/FPE) on an
/// alternate signal stack. On a fault the handler async-signal-safely
/// writes the signal name, fault address, last command, and a backtrace
/// to `log_path` (appended) and to stderr, then restores the signal's
/// default disposition and re-raises it so the kernel still produces a
/// core dump. A crash is thereby always diagnosed, never silent.
/// @param log_path File to append crash diagnostics to. If empty, only
///   stderr is written.
/// @returns true if the handlers were installed.
auto InstallFaultHandlers(const std::string &log_path) -> bool;

/// Ignore SIGPIPE process-wide. A peer (ZMQ socket, SSH pipe)
/// disconnecting mid-write otherwise delivers SIGPIPE, whose default
/// action kills the process — a trivial remote DoS. Idempotent.
auto IgnoreSigpipe() -> void;

/// Callbacks the control-signal listener invokes. Each runs on the
/// listener's own thread in a normal context, so it may allocate, take
/// locks, and log. Any callback may be empty (the signal is then
/// drained and ignored).
struct ControlHandlers {
  /// TERM / INT / QUIT / HUP — begin a graceful shutdown: stop taking
  /// input, flush, release resources, exit cleanly. For the CLI, HUP
  /// means the SSH session dropped; the CLI exits, and (by design) the
  /// rollback timer lives in the daemon, not here, so it survives.
  std::function<void()> on_shutdown;
  /// USR1 — reopen log files (logrotate).
  std::function<void()> on_reopen_logs;
  /// USR2 — dump runtime status / stats to the log without stopping.
  std::function<void()> on_dump_status;
};

/// Blocks the async control signals in the calling thread (threads
/// spawned afterwards inherit the block) and runs a dedicated thread
/// that drains them from a `signalfd`, dispatching each to `handlers`.
/// Construct this early, before spawning worker threads, so no thread
/// retains a default disposition for these signals. Stops the thread
/// and restores the mask on destruction (RAII).
class ControlListener {
 public:
  /// @param handlers Callbacks to invoke per signal. Copied.
  explicit ControlListener(ControlHandlers handlers);
  ~ControlListener();

  ControlListener(const ControlListener &) = delete;
  auto operator=(const ControlListener &) -> ControlListener & = delete;
  ControlListener(ControlListener &&) = delete;
  auto operator=(ControlListener &&) -> ControlListener & = delete;

  /// Whether the listener thread started successfully.
  auto Running() const -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace einheit::cli::signals

#endif  // INCLUDE_EINHEIT_CLI_SIGNALS_H_
