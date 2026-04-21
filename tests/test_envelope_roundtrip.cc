/// @file test_envelope_roundtrip.cc
/// @brief Round-trip encode/decode tests for Request and Response.
// Copyright (c) 2026 Einheit Networks

#include <gtest/gtest.h>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/protocol/msgpack_codec.h"

namespace einheit::cli::protocol {

TEST(Envelope, RequestRoundTrip) {
  Request req;
  req.id = "abc-123";
  req.user = "karl";
  req.role = "admin";
  req.command = "show";
  req.args = {"tunnels"};

  auto bytes = EncodeRequest(req);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().message;

  auto decoded = DecodeRequest(*bytes);
  ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

  EXPECT_EQ(decoded->id, req.id);
  EXPECT_EQ(decoded->user, req.user);
  EXPECT_EQ(decoded->role, req.role);
  EXPECT_EQ(decoded->command, req.command);
  EXPECT_EQ(decoded->args, req.args);
}

TEST(Envelope, ResponseOkRoundTrip) {
  Response res;
  res.id = "abc-123";
  res.status = ResponseStatus::Ok;
  res.data = {0x01, 0x02, 0x03};

  auto bytes = EncodeResponse(res);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
  auto decoded = DecodeResponse(*bytes);
  ASSERT_TRUE(decoded.has_value()) << decoded.error().message;

  EXPECT_EQ(decoded->id, res.id);
  EXPECT_EQ(decoded->status, ResponseStatus::Ok);
  EXPECT_EQ(decoded->data, res.data);
  EXPECT_FALSE(decoded->error.has_value());
}

TEST(Envelope, ResponseErrorRoundTrip) {
  Response res;
  res.id = "abc-123";
  res.status = ResponseStatus::Error;
  res.error = ResponseError{"policy_denied", "denied", "check role"};

  auto bytes = EncodeResponse(res);
  ASSERT_TRUE(bytes.has_value());
  auto decoded = DecodeResponse(*bytes);
  ASSERT_TRUE(decoded.has_value());

  EXPECT_EQ(decoded->status, ResponseStatus::Error);
  ASSERT_TRUE(decoded->error.has_value());
  EXPECT_EQ(decoded->error->code, "policy_denied");
  EXPECT_EQ(decoded->error->message, "denied");
  EXPECT_EQ(decoded->error->hint, "check role");
}

}  // namespace einheit::cli::protocol
