/// @file audit.cc
/// @brief Client-side identity stamping. The authoritative audit log
/// is written by the daemon.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/audit.h"

#include <chrono>
#include <ctime>
#include <format>

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

auto NowTimestamp() -> std::string {
  const auto now = std::chrono::system_clock::now();
  const auto secs =
      std::chrono::floor<std::chrono::seconds>(now);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - secs)
          .count();
  const std::time_t t = std::chrono::system_clock::to_time_t(secs);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  return std::format(
      "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", tm.tm_year + 1900,
      tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

}  // namespace einheit::cli::audit
