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
  uid_t uid = ::getuid();
  struct passwd *pw = ::getpwuid(uid);
  if (!pw) {
    return std::unexpected(Error<AuthError>{
        AuthError::UnknownUser, "getpwuid failed"});
  }
  id.user = pw->pw_name;
  // Role mapping comes from daemon config; default to read-only
  // until the daemon responds. Daemon re-stamps the authoritative
  // role on each Response. EINHEIT_ROLE (admin|operator|any) opts
  // the local caller into a higher gate for local-IPC scenarios.
  id.role = RoleFromEnv();
  id.transport = ::isatty(STDIN_FILENO) ? "tty" : "pipe";
  return id;
}

}  // namespace einheit::cli::auth
