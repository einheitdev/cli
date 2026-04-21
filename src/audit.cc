/// @file audit.cc
/// @brief Client-side identity stamping. The authoritative audit log
/// is written by the daemon.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/audit.h"

namespace einheit::cli::audit {
namespace {

auto RoleToString(RoleGate role) -> std::string {
  switch (role) {
    case RoleGate::AdminOnly:         return "admin";
    case RoleGate::OperatorOrAdmin:   return "operator";
    case RoleGate::AnyAuthenticated:
    default:                          return "readonly";
  }
}

}  // namespace

auto StampIdentity(const auth::CallerIdentity &caller,
                   std::string &user_out, std::string &role_out)
    -> void {
  user_out = caller.user;
  role_out = RoleToString(caller.role);
}

}  // namespace einheit::cli::audit
