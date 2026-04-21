/// @file msgpack_codec.cc
/// @brief MessagePack encode/decode implementation.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/protocol/msgpack_codec.h"

#include <exception>
#include <string>

#include <msgpack.hpp>

namespace einheit::cli::protocol {
namespace {

auto MakeError(CodecError code, std::string message)
    -> Error<CodecError> {
  return Error<CodecError>{code, std::move(message)};
}

}  // namespace

auto EncodeRequest(const Request &req)
    -> std::expected<std::vector<std::uint8_t>, Error<CodecError>> {
  try {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);

    pk.pack_map(7);
    pk.pack(std::string("envelope_version"));
    pk.pack(req.envelope_version);
    pk.pack(std::string("id"));
    pk.pack(req.id);
    pk.pack(std::string("user"));
    pk.pack(req.user);
    pk.pack(std::string("role"));
    pk.pack(req.role);
    pk.pack(std::string("session_id"));
    if (req.session_id) {
      pk.pack(*req.session_id);
    } else {
      pk.pack_nil();
    }
    pk.pack(std::string("command"));
    pk.pack(req.command);
    pk.pack(std::string("args"));
    pk.pack(req.args);

    const auto *bytes =
        reinterpret_cast<const std::uint8_t *>(sbuf.data());
    return std::vector<std::uint8_t>(bytes, bytes + sbuf.size());
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(CodecError::ExceptionRaised, e.what()));
  }
}

auto DecodeRequest(std::span<const std::uint8_t> bytes)
    -> std::expected<Request, Error<CodecError>> {
  try {
    msgpack::object_handle oh = msgpack::unpack(
        reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const msgpack::object &obj = oh.get();
    if (obj.type != msgpack::type::MAP) {
      return std::unexpected(
          MakeError(CodecError::TypeMismatch, "request not a map"));
    }
    Request out;
    for (std::uint32_t i = 0; i < obj.via.map.size; ++i) {
      const auto &kv = obj.via.map.ptr[i];
      std::string key;
      kv.key.convert(key);
      if (key == "envelope_version") {
        kv.val.convert(out.envelope_version);
      } else if (key == "id") {
        kv.val.convert(out.id);
      } else if (key == "user") {
        kv.val.convert(out.user);
      } else if (key == "role") {
        kv.val.convert(out.role);
      } else if (key == "session_id") {
        if (kv.val.type != msgpack::type::NIL) {
          std::string sid;
          kv.val.convert(sid);
          out.session_id = std::move(sid);
        }
      } else if (key == "command") {
        kv.val.convert(out.command);
      } else if (key == "args") {
        kv.val.convert(out.args);
      }
    }
    if (out.envelope_version != kEnvelopeVersion) {
      return std::unexpected(MakeError(
          CodecError::VersionMismatch, "envelope_version mismatch"));
    }
    return out;
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(CodecError::ExceptionRaised, e.what()));
  }
}

auto EncodeResponse(const Response &res)
    -> std::expected<std::vector<std::uint8_t>, Error<CodecError>> {
  try {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);

    pk.pack_map(5);
    pk.pack(std::string("envelope_version"));
    pk.pack(res.envelope_version);
    pk.pack(std::string("id"));
    pk.pack(res.id);
    pk.pack(std::string("status"));
    pk.pack(std::string(
        res.status == ResponseStatus::Ok ? "ok" : "error"));
    pk.pack(std::string("data"));
    pk.pack_bin(res.data.size());
    if (!res.data.empty()) {
      pk.pack_bin_body(
          reinterpret_cast<const char *>(res.data.data()),
          res.data.size());
    }
    pk.pack(std::string("error"));
    if (res.error) {
      pk.pack_map(3);
      pk.pack(std::string("code"));
      pk.pack(res.error->code);
      pk.pack(std::string("message"));
      pk.pack(res.error->message);
      pk.pack(std::string("hint"));
      pk.pack(res.error->hint);
    } else {
      pk.pack_nil();
    }

    const auto *bytes =
        reinterpret_cast<const std::uint8_t *>(sbuf.data());
    return std::vector<std::uint8_t>(bytes, bytes + sbuf.size());
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(CodecError::ExceptionRaised, e.what()));
  }
}

auto DecodeResponse(std::span<const std::uint8_t> bytes)
    -> std::expected<Response, Error<CodecError>> {
  try {
    msgpack::object_handle oh = msgpack::unpack(
        reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const msgpack::object &obj = oh.get();
    if (obj.type != msgpack::type::MAP) {
      return std::unexpected(MakeError(
          CodecError::TypeMismatch, "response not a map"));
    }
    Response out;
    for (std::uint32_t i = 0; i < obj.via.map.size; ++i) {
      const auto &kv = obj.via.map.ptr[i];
      std::string key;
      kv.key.convert(key);
      if (key == "envelope_version") {
        kv.val.convert(out.envelope_version);
      } else if (key == "id") {
        kv.val.convert(out.id);
      } else if (key == "status") {
        std::string s;
        kv.val.convert(s);
        out.status = (s == "ok") ? ResponseStatus::Ok
                                 : ResponseStatus::Error;
      } else if (key == "data") {
        if (kv.val.type == msgpack::type::BIN) {
          const auto *p = reinterpret_cast<const std::uint8_t *>(
              kv.val.via.bin.ptr);
          out.data.assign(p, p + kv.val.via.bin.size);
        }
      } else if (key == "error") {
        if (kv.val.type == msgpack::type::MAP) {
          ResponseError err;
          for (std::uint32_t j = 0; j < kv.val.via.map.size; ++j) {
            const auto &ekv = kv.val.via.map.ptr[j];
            std::string ekey;
            ekv.key.convert(ekey);
            if (ekey == "code") {
              ekv.val.convert(err.code);
            } else if (ekey == "message") {
              ekv.val.convert(err.message);
            } else if (ekey == "hint") {
              ekv.val.convert(err.hint);
            }
          }
          out.error = std::move(err);
        }
      }
    }
    if (out.envelope_version != kEnvelopeVersion) {
      return std::unexpected(MakeError(
          CodecError::VersionMismatch, "envelope_version mismatch"));
    }
    return out;
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(CodecError::ExceptionRaised, e.what()));
  }
}

auto EncodeEventBody(const Event &ev)
    -> std::expected<std::vector<std::uint8_t>, Error<CodecError>> {
  try {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);
    pk.pack_map(3);
    pk.pack(std::string("envelope_version"));
    pk.pack(ev.envelope_version);
    pk.pack(std::string("timestamp"));
    pk.pack(ev.timestamp);
    pk.pack(std::string("data"));
    pk.pack_bin(ev.data.size());
    if (!ev.data.empty()) {
      pk.pack_bin_body(
          reinterpret_cast<const char *>(ev.data.data()),
          ev.data.size());
    }
    const auto *bytes =
        reinterpret_cast<const std::uint8_t *>(sbuf.data());
    return std::vector<std::uint8_t>(bytes, bytes + sbuf.size());
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(CodecError::ExceptionRaised, e.what()));
  }
}

auto DecodeEventBody(const std::string &topic,
                     std::span<const std::uint8_t> bytes)
    -> std::expected<Event, Error<CodecError>> {
  try {
    msgpack::object_handle oh = msgpack::unpack(
        reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const msgpack::object &obj = oh.get();
    if (obj.type != msgpack::type::MAP) {
      return std::unexpected(MakeError(
          CodecError::TypeMismatch, "event body not a map"));
    }
    Event out;
    out.topic = topic;
    for (std::uint32_t i = 0; i < obj.via.map.size; ++i) {
      const auto &kv = obj.via.map.ptr[i];
      std::string key;
      kv.key.convert(key);
      if (key == "envelope_version") {
        kv.val.convert(out.envelope_version);
      } else if (key == "timestamp") {
        kv.val.convert(out.timestamp);
      } else if (key == "data") {
        if (kv.val.type == msgpack::type::BIN) {
          const auto *p = reinterpret_cast<const std::uint8_t *>(
              kv.val.via.bin.ptr);
          out.data.assign(p, p + kv.val.via.bin.size);
        }
      }
    }
    return out;
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(CodecError::ExceptionRaised, e.what()));
  }
}

}  // namespace einheit::cli::protocol
