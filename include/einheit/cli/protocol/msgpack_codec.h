/// @file msgpack_codec.h
/// @brief MessagePack encode/decode for the wire protocol envelopes.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_PROTOCOL_MSGPACK_CODEC_H_
#define INCLUDE_EINHEIT_CLI_PROTOCOL_MSGPACK_CODEC_H_

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "einheit/cli/error.h"
#include "einheit/cli/protocol/envelope.h"

namespace einheit::cli::protocol {

/// Errors surfaced by the codec.
enum class CodecError {
  /// Buffer underrun / truncated frame.
  Truncated,
  /// Field type mismatch during decode.
  TypeMismatch,
  /// envelope_version did not match kEnvelopeVersion.
  VersionMismatch,
  /// A required field was missing.
  MissingField,
  /// Underlying msgpack library raised an unexpected exception.
  ExceptionRaised,
};

/// Serialise a Request to its on-wire MessagePack representation.
/// @param req Request to encode.
/// @returns MessagePack byte buffer, or CodecError.
auto EncodeRequest(const Request &req)
    -> std::expected<std::vector<std::uint8_t>, Error<CodecError>>;

/// Parse a MessagePack buffer into a Request.
/// @param bytes MessagePack bytes received on the wire.
/// @returns Decoded Request, or CodecError.
auto DecodeRequest(std::span<const std::uint8_t> bytes)
    -> std::expected<Request, Error<CodecError>>;

/// Serialise a Response to its on-wire MessagePack representation.
/// @param res Response to encode.
/// @returns MessagePack byte buffer, or CodecError.
auto EncodeResponse(const Response &res)
    -> std::expected<std::vector<std::uint8_t>, Error<CodecError>>;

/// Parse a MessagePack buffer into a Response.
/// @param bytes MessagePack bytes received on the wire.
/// @returns Decoded Response, or CodecError.
auto DecodeResponse(std::span<const std::uint8_t> bytes)
    -> std::expected<Response, Error<CodecError>>;

/// Encode an Event body (the second ZMQ frame; the topic string is
/// sent separately as the first frame).
/// @param ev Event to encode.
/// @returns MessagePack byte buffer, or CodecError.
auto EncodeEventBody(const Event &ev)
    -> std::expected<std::vector<std::uint8_t>, Error<CodecError>>;

/// Decode an Event body from its MessagePack buffer.
/// @param topic Topic string from the first ZMQ frame.
/// @param bytes Body bytes from the second ZMQ frame.
/// @returns Decoded Event, or CodecError.
auto DecodeEventBody(const std::string &topic,
                     std::span<const std::uint8_t> bytes)
    -> std::expected<Event, Error<CodecError>>;

}  // namespace einheit::cli::protocol

#endif  // INCLUDE_EINHEIT_CLI_PROTOCOL_MSGPACK_CODEC_H_
