/// @file auth.cc
/// @brief Client-side auth resolution (local UID -> einheit user).
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/auth.h"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace einheit::cli::auth {

namespace {
auto RoleFromEnv() -> RoleGate {
  const char *v = std::getenv("EINHEIT_ROLE");
  if (!v || !*v) return RoleGate::AnyAuthenticated;
  std::string_view s{v};
  if (s == "admin") return RoleGate::AdminOnly;
  if (s == "operator") return RoleGate::OperatorOrAdmin;
  return RoleGate::AnyAuthenticated;
}
}  // namespace

auto ResolveLocal()
    -> std::expected<CallerIdentity, Error<AuthError>> {
  CallerIdentity id;
  // EINHEIT_USER lets a trusted launcher (e.g. einheit-shell-
  // launcher) preserve the operator's identity after dropping
  // privs to a dedicated sandbox uid. Without this hook every
  // request would be stamped with the sandbox uid's pw_name and
  // the audit chain would lose the real caller.
  if (const char *forced = std::getenv("EINHEIT_USER");
      forced && *forced) {
    id.user = forced;
  } else {
    uid_t uid = ::getuid();
    struct passwd *pw = ::getpwuid(uid);
    if (!pw) {
      return std::unexpected(Error<AuthError>{
          AuthError::UnknownUser, "getpwuid failed"});
    }
    id.user = pw->pw_name;
  }
  // Role mapping comes from daemon config; default to read-only
  // until the daemon responds. Daemon re-stamps the authoritative
  // role on each Response. EINHEIT_ROLE (admin|operator|any) opts
  // the local caller into a higher gate for local-IPC scenarios.
  id.role = RoleFromEnv();
  id.transport = ::isatty(STDIN_FILENO) ? "tty" : "pipe";
  return id;
}

}  // namespace einheit::cli::auth
