/// @file shell_escape.cc
/// @brief POSIX shell escape with bracketed audit notification.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/shell_escape.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <format>
#include <string>
#include <utility>

#include "einheit/cli/audit.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"

namespace einheit::cli::shell_escape {
namespace {

auto MakeError(EscapeError code, std::string message)
    -> Error<EscapeError> {
  return Error<EscapeError>{code, std::move(message)};
}

auto NotifyDaemon(transport::Transport &tx,
                  const auth::CallerIdentity &caller,
                  const std::string &boundary)
    -> std::expected<void, Error<EscapeError>> {
  protocol::Request req;
  req.command = boundary;
  audit::StampIdentity(caller, req.user, req.role);
  using namespace std::chrono_literals;
  auto resp = tx.SendRequest(req, 2s);
  if (!resp) {
    return std::unexpected(MakeError(
        EscapeError::AuditFailed, resp.error().message));
  }
  return {};
}

auto ResolveShellPath() -> std::string {
  if (const char *env = std::getenv("SHELL"); env && *env) {
    return env;
  }
  return "/bin/bash";
}

}  // namespace

auto Escape(transport::Transport &tx,
            const auth::CallerIdentity &caller, const Hooks &hooks)
    -> std::expected<int, Error<EscapeError>> {
  if (caller.role != RoleGate::AdminOnly) {
    return std::unexpected(MakeError(
        EscapeError::NotAuthorised,
        "shell escape requires admin role"));
  }

  if (auto r = NotifyDaemon(tx, caller, "shell_enter"); !r) {
    return std::unexpected(r.error());
  }

  const auto bash = ResolveShellPath();
  int status = 0;

  if (hooks.run_shell) {
    status = hooks.run_shell(bash);
  } else {
    pid_t pid = ::fork();
    if (pid < 0) {
      return std::unexpected(MakeError(
          EscapeError::ExecFailed, "fork failed"));
    }
    if (pid == 0) {
      ::execl(bash.c_str(), bash.c_str(),
              static_cast<char *>(nullptr));
      std::_Exit(127);
    }
    if (::waitpid(pid, &status, 0) < 0) {
      return std::unexpected(MakeError(
          EscapeError::ExecFailed, "waitpid failed"));
    }
    if (WIFEXITED(status)) status = WEXITSTATUS(status);
  }

  if (auto r = NotifyDaemon(tx, caller, "shell_exit"); !r) {
    return std::unexpected(r.error());
  }
  return status;
}

}  // namespace einheit::cli::shell_escape
