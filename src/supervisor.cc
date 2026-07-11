/// @file supervisor.cc
/// @brief Crash supervisor implementation.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/supervisor.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <format>
#include <iostream>

#include "einheit/cli/signals.h"

namespace einheit::cli {
namespace {

auto SignalName(int sig) -> const char * {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGTERM: return "SIGTERM";
    case SIGINT:  return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGHUP:  return "SIGHUP";
    default:      return "signal";
  }
}

constexpr std::size_t kSharedCmdMax = 512;

}  // namespace

auto IsFaultSignal(int sig) -> bool {
  return sig == SIGSEGV || sig == SIGABRT || sig == SIGBUS ||
         sig == SIGILL || sig == SIGFPE;
}

auto RunSupervised(const std::function<int()> &child_main,
                   const SupervisorOptions &opts,
                   SupervisorResult *result) -> int {
  std::ostream &out = opts.out ? *opts.out : std::cerr;

  // Shared page the child mirrors its last command into, so the parent
  // can name it after the child dies. MAP_ANONYMOUS|MAP_SHARED means
  // both processes see the same physical page across fork.
  char *shared = static_cast<char *>(
      ::mmap(nullptr, kSharedCmdMax, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  if (shared == MAP_FAILED) shared = nullptr;
  if (shared != nullptr) shared[0] = '\0';

  const pid_t pid = ::fork();
  if (pid < 0) {
    // Can't fork — run the shell inline rather than refuse to start.
    // No supervision, but the fault handler still logs a crash.
    if (shared != nullptr) ::munmap(shared, kSharedCmdMax);
    return child_main();
  }

  if (pid == 0) {
    // Child: mirror the last command into shared memory, run the shell,
    // exit with its status. A fault re-raises via the inherited handler
    // and the child dies from the signal.
    if (shared != nullptr) {
      signals::MirrorLastCommandTo(shared, kSharedCmdMax);
    }
    ::_exit(child_main());
  }

  // Parent: forward a graceful-termination signal to the child so the
  // supervisor never orphans it, then reap. The listener runs on its
  // own thread; started here (post-fork) so the fork stays
  // single-threaded.
  signals::ControlHandlers handlers;
  handlers.on_shutdown = [pid] {
    ::kill(pid, SIGTERM);
  };
  // Ctrl-C reaches the child directly through the foreground
  // process group; the supervisor only drains it. Forwarding it as
  // SIGTERM (the old behaviour) turned every interactive interrupt
  // into a session logout.
  handlers.on_interrupt = [] {};
  signals::ControlListener listener(std::move(handlers));

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  SupervisorResult r;
  if (WIFEXITED(status)) {
    r.outcome = ChildOutcome::Exited;
    r.exit_code = WEXITSTATUS(status);
    if (result) *result = r;
    return r.exit_code;
  }

  // Killed by a signal.
  const int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
  r.signal = sig;
  const std::string last_command =
      shared != nullptr ? std::string(
                              shared, ::strnlen(shared, kSharedCmdMax))
                        : std::string();
  r.last_command = last_command;

  if (IsFaultSignal(sig)) {
    r.outcome = ChildOutcome::Crashed;
    // Name the last command when we recovered it (in-process fork);
    // after a re-exec it lives in the crash log instead.
    if (last_command.empty()) {
      out << std::format("\neinheit: the CLI crashed ({}).\n",
                         SignalName(sig));
    } else {
      out << std::format(
          "\neinheit: the CLI crashed running '{}' ({}).\n",
          last_command, SignalName(sig));
    }
    if (!opts.crash_log_path.empty()) {
      out << std::format("  Diagnostics (last command + backtrace) "
                         "logged to {}.\n",
                         opts.crash_log_path);
    }
    out << "  The session is ending cleanly rather than leaving you at "
           "a dead prompt.\n";
  } else {
    r.outcome = ChildOutcome::Terminated;
    out << std::format("\neinheit: the CLI was terminated ({}).\n",
                       SignalName(sig));
  }

  if (shared != nullptr) ::munmap(shared, kSharedCmdMax);
  if (result) *result = r;
  return 128 + sig;
}

}  // namespace einheit::cli
