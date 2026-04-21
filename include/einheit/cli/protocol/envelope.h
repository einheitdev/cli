/// @file envelope.h
/// @brief Wire-protocol envelopes — Request, Response, Event.
///
/// These are plain data (data-oriented): free functions in
/// msgpack_codec.h encode and decode them. Keep fields flat and POD
/// where possible so MessagePack mapping is trivial.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_PROTOCOL_ENVELOPE_H_
#define INCLUDE_EINHEIT_CLI_PROTOCOL_ENVELOPE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace einheit::cli::protocol {

/// Current envelope schema version. Bump on incompatible changes.
inline constexpr std::uint32_t kEnvelopeVersion = 1;

/// Request envelope — CLI to daemon on the .ctl socket.
struct Request {
  /// Wire schema version; must equal kEnvelopeVersion.
  std::uint32_t envelope_version = kEnvelopeVersion;
  /// Correlation id (UUIDv4); echoed in the matching Response.
  std::string id;
  /// Caller user name. Advisory — the daemon trusts SO_PEERCRED /
  /// the CurveZMQ-authenticated key, not this field.
  std::string user;
  /// Caller role at dispatch time. Also advisory.
  std::string role;
  /// Candidate-config session id, or empty if not in configure mode.
  std::optional<std::string> session_id;
  /// Top-level command verb, e.g. "show", "set", "commit".
  std::string command;
  /// Positional arguments.
  std::vector<std::string> args;
  /// Named flags (e.g. {"format": "json"}).
  std::unordered_map<std::string, std::string> flags;
};

/// Response status discriminator.
enum class ResponseStatus {
  /// Command executed successfully.
  Ok,
  /// Command was rejected or failed.
  Error,
};

/// Structured error body inside a Response.
struct ResponseError {
  /// Machine-readable error code (daemon-defined).
  std::string code;
  /// Human-readable error text shown to the operator.
  std::string message;
  /// Optional actionable hint ("did you mean X?", etc.).
  std::string hint;
};

/// Response envelope — daemon to CLI on the .ctl socket.
struct Response {
  /// Wire schema version.
  std::uint32_t envelope_version = kEnvelopeVersion;
  /// Echoes the Request::id that produced this reply.
  std::string id;
  /// Ok / Error.
  ResponseStatus status = ResponseStatus::Ok;
  /// Raw MessagePack blob of command-specific response data.
  /// The adapter's renderer decodes this.
  std::vector<std::uint8_t> data;
  /// Populated iff status == ResponseStatus::Error.
  std::optional<ResponseError> error;
};

/// Event envelope — daemon to subscribers on the .pub socket.
/// Arrives as two ZMQ frames: topic (UTF-8 string) and data (msgpack).
struct Event {
  /// Hierarchical topic ("state.tunnels.munich-gw"). ZMQ prefix-
  /// filters on this frame.
  std::string topic;
  /// Wire schema version of the body.
  std::uint32_t envelope_version = kEnvelopeVersion;
  /// Event timestamp, RFC 3339 with millisecond precision.
  std::string timestamp;
  /// MessagePack-encoded event payload.
  std::vector<std::uint8_t> data;
};

}  // namespace einheit::cli::protocol

#endif  // INCLUDE_EINHEIT_CLI_PROTOCOL_ENVELOPE_H_
