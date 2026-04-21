/// @file test_net_parse.cc
/// @brief IPv4/IPv6 and CIDR parsing tests.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include "einheit/cli/net_parse.h"

namespace einheit::cli::net_parse {

TEST(NetParse, IpV4Accepts) {
  EXPECT_TRUE(IsIpAddress("10.0.0.1"));
  EXPECT_TRUE(IsIpAddress("0.0.0.0"));
  EXPECT_TRUE(IsIpAddress("255.255.255.255"));
}

TEST(NetParse, IpV4Rejects) {
  EXPECT_FALSE(IsIpAddress("10.0.0"));
  EXPECT_FALSE(IsIpAddress("10.0.0.256"));
  EXPECT_FALSE(IsIpAddress("not an ip"));
  EXPECT_FALSE(IsIpAddress(""));
}

TEST(NetParse, IpV6Accepts) {
  EXPECT_TRUE(IsIpAddress("::1"));
  EXPECT_TRUE(IsIpAddress("fe80::1"));
  EXPECT_TRUE(IsIpAddress("2001:db8::1"));
  EXPECT_TRUE(IsIpAddress(
      "2001:0db8:85a3:0000:0000:8a2e:0370:7334"));
}

TEST(NetParse, IpV6Rejects) {
  EXPECT_FALSE(IsIpAddress("2001:db8::g"));
  EXPECT_FALSE(IsIpAddress(":::1"));
}

TEST(NetParse, CidrV4Accepts) {
  EXPECT_TRUE(IsCidr("10.0.0.0/24"));
  EXPECT_TRUE(IsCidr("0.0.0.0/0"));
  EXPECT_TRUE(IsCidr("192.168.1.1/32"));
}

TEST(NetParse, CidrV4RejectsBadPrefix) {
  EXPECT_FALSE(IsCidr("10.0.0.0/33"));
  EXPECT_FALSE(IsCidr("10.0.0.0/-1"));
  EXPECT_FALSE(IsCidr("10.0.0.0/"));
  EXPECT_FALSE(IsCidr("10.0.0.0/abc"));
}

TEST(NetParse, CidrV6Accepts) {
  EXPECT_TRUE(IsCidr("::/0"));
  EXPECT_TRUE(IsCidr("fe80::/64"));
  EXPECT_TRUE(IsCidr("2001:db8::/32"));
}

TEST(NetParse, CidrV6RejectsBadPrefix) {
  EXPECT_FALSE(IsCidr("::/129"));
  EXPECT_FALSE(IsCidr("2001:db8::/"));
}

TEST(NetParse, CidrRejectsMissingSlash) {
  EXPECT_FALSE(IsCidr("10.0.0.0"));
  EXPECT_FALSE(IsCidr("::1"));
}

}  // namespace einheit::cli::net_parse
