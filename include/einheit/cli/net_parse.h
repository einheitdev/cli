/// @file net_parse.h
/// @brief IPv4/IPv6 address + CIDR parsing, built on inet_pton. Used
/// by the schema validator for primitive types `ip_address` and
/// `cidr`, and by the remote transport for endpoint pinning.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_NET_PARSE_H_
#define INCLUDE_EINHEIT_CLI_NET_PARSE_H_

#include <cstdint>
#include <string>

namespace einheit::cli::net_parse {

/// True iff `s` is a valid IPv4 or IPv6 literal (no CIDR suffix).
/// @param s Candidate address.
/// @returns True on success.
auto IsIpAddress(const std::string &s) -> bool;

/// True iff `s` is a valid CIDR: IPv4/IPv6 literal + `/<prefix>`
/// where prefix is in [0, 32] for v4 and [0, 128] for v6.
/// @param s Candidate CIDR.
/// @returns True on success.
auto IsCidr(const std::string &s) -> bool;

}  // namespace einheit::cli::net_parse

#endif  // INCLUDE_EINHEIT_CLI_NET_PARSE_H_
