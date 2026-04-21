/// @file net_parse.cc
/// @brief IPv4/IPv6 + CIDR parsing via inet_pton.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/net_parse.h"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace einheit::cli::net_parse {
namespace {

auto ParseUnsigned(const std::string &s, std::uint32_t max_value,
                   std::uint32_t &out) -> bool {
  if (s.empty()) return false;
  std::uint64_t n = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    n = n * 10 + static_cast<std::uint64_t>(c - '0');
    if (n > max_value) return false;
  }
  out = static_cast<std::uint32_t>(n);
  return true;
}

auto IsV4(const std::string &s) -> bool {
  struct in_addr addr4{};
  return ::inet_pton(AF_INET, s.c_str(), &addr4) == 1;
}

auto IsV6(const std::string &s) -> bool {
  struct in6_addr addr6{};
  return ::inet_pton(AF_INET6, s.c_str(), &addr6) == 1;
}

}  // namespace

auto IsIpAddress(const std::string &s) -> bool {
  return IsV4(s) || IsV6(s);
}

auto IsCidr(const std::string &s) -> bool {
  const auto slash = s.find('/');
  if (slash == std::string::npos) return false;

  const std::string addr = s.substr(0, slash);
  const std::string prefix = s.substr(slash + 1);

  const bool v4 = IsV4(addr);
  const bool v6 = v4 ? false : IsV6(addr);
  if (!v4 && !v6) return false;

  std::uint32_t p = 0;
  if (!ParseUnsigned(prefix, v4 ? 32u : 128u, p)) return false;
  return true;
}

}  // namespace einheit::cli::net_parse
