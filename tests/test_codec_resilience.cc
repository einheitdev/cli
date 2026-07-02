/// @file test_codec_resilience.cc
/// @brief Malformed / hostile / version-bumped input exercised
/// against the MessagePack codec. The decoder must always return a
/// typed error, never throw, never segfault.
// Copyright (c) 2026 Einheit Networks

#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <gtest/gtest.h>
#include <msgpack.hpp>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/protocol/msgpack_codec.h"

namespace einheit::cli::protocol {

TEST(CodecResilience, EmptyBufferDoesNotCrash) {
  std::vector<std::uint8_t> empty;
  auto r = DecodeRequest(empty);
  EXPECT_FALSE(r.has_value());
}

TEST(CodecResilience, RandomBytesReturnError) {
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> byte_val(0, 255);
  for (int seed = 0; seed < 200; ++seed) {
    std::vector<std::uint8_t> junk(32 + (seed % 64));
    for (auto &b : junk) {
      b = static_cast<std::uint8_t>(byte_val(rng));
    }
    // Must not throw; either returns an error or a decoded frame
    // that fails the version guard.
    auto r = DecodeRequest(junk);
    if (r.has_value()) {
      EXPECT_EQ(r->envelope_version, kEnvelopeVersion);
    }
    auto r2 = DecodeResponse(junk);
    if (r2.has_value()) {
      EXPECT_EQ(r2->envelope_version, kEnvelopeVersion);
    }
  }
}

TEST(CodecResilience, TruncatedRequestReturnsError) {
  Request req;
  req.id = "x";
  req.command = "show";
  auto encoded = EncodeRequest(req);
  ASSERT_TRUE(encoded.has_value());

  // Drop the last third of the buffer.
  encoded->resize(encoded->size() * 2 / 3);
  auto r = DecodeRequest(*encoded);
  EXPECT_FALSE(r.has_value());
}

TEST(CodecResilience, TruncatedResponseReturnsError) {
  Response res;
  res.id = "x";
  res.status = ResponseStatus::Ok;
  auto encoded = EncodeResponse(res);
  ASSERT_TRUE(encoded.has_value());
  encoded->resize(encoded->size() / 2);
  auto r = DecodeResponse(*encoded);
  EXPECT_FALSE(r.has_value());
}

TEST(CodecResilience, UnknownVersionRejected) {
  // Hand-craft a MessagePack map with a future envelope_version.
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  pk.pack_map(7);
  pk.pack(std::string("envelope_version"));
  pk.pack(std::uint32_t{9999});
  pk.pack(std::string("id"));
  pk.pack(std::string("x"));
  pk.pack(std::string("user"));
  pk.pack(std::string(""));
  pk.pack(std::string("role"));
  pk.pack(std::string(""));
  pk.pack(std::string("session_id"));
  pk.pack_nil();
  pk.pack(std::string("command"));
  pk.pack(std::string("show"));
  pk.pack(std::string("args"));
  pk.pack(std::vector<std::string>{});

  const auto *p =
      reinterpret_cast<const std::uint8_t *>(sbuf.data());
  std::span<const std::uint8_t> s(p, sbuf.size());
  auto r = DecodeRequest(s);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, CodecError::VersionMismatch);
}

TEST(CodecResilience, NonMapPayloadRejected) {
  // MessagePack for the integer 42: 0x2A.
  std::vector<std::uint8_t> buf{0x2A};
  auto r = DecodeRequest(buf);
  EXPECT_FALSE(r.has_value());
}

TEST(CodecResilience, HugeLengthPrefixDoesNotExhaustMemory) {
  // A 5-byte array32 header claiming ~2 billion elements, with no
  // body. An unbounded decoder pre-allocates ~32 GB here (ASan aborts
  // with out-of-memory); the length-derived unpack bound must reject
  // it as a clean typed error instead. Real DoS surface: a malformed
  // daemon reply must never exhaust CLI memory.
  std::vector<std::uint8_t> hostile_array{0xdd, 0x7f, 0xff, 0xff, 0xff};
  auto r = DecodeRequest(hostile_array);
  EXPECT_FALSE(r.has_value());
  auto r2 = DecodeResponse(hostile_array);
  EXPECT_FALSE(r2.has_value());

  // Same attack via a str32 header claiming a ~4 GB string.
  std::vector<std::uint8_t> hostile_str{0xdb, 0xff, 0xff, 0xff, 0xff};
  auto r3 = DecodeRequest(hostile_str);
  EXPECT_FALSE(r3.has_value());

  // And a map32 header claiming ~4 billion pairs.
  std::vector<std::uint8_t> hostile_map{0xdf, 0xff, 0xff, 0xff, 0xff};
  auto r4 = DecodeRequest(hostile_map);
  EXPECT_FALSE(r4.has_value());
}

TEST(CodecResilience, EventBodyRoundTripSurvivesBinaryPayload) {
  Event ev;
  ev.timestamp = "2026-04-20T00:00:00.000Z";
  ev.data.assign({0x00, 0xFF, 0x7F, 0x80, 0x01});
  auto bytes = EncodeEventBody(ev);
  ASSERT_TRUE(bytes.has_value());
  auto decoded = DecodeEventBody("topic", *bytes);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->data, ev.data);
  EXPECT_EQ(decoded->topic, "topic");
}

}  // namespace einheit::cli::protocol
