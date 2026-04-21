/// @file auth.cc
/// @brief Client-side auth resolution (local UID -> einheit user).
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/auth.h"

#include <pwd.h>
#include <unistd.h>

#include <string>

namespace einheit::cli::auth {

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
  // role on each Response.
  id.role = RoleGate::AnyAuthenticated;
  id.transport = ::isatty(STDIN_FILENO) ? "tty" : "pipe";
  return id;
}

}  // namespace einheit::cli::auth
