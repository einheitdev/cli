/// @file curve_keys.cc
/// @brief CurveZMQ keypair generation + file I/O.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/curve_keys.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <array>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>

#include <zmq.h>

namespace einheit::cli::curve {
namespace {

auto MakeError(KeyError code, std::string message)
    -> Error<KeyError> {
  return Error<KeyError>{code, std::move(message)};
}

// Z85-encoded keys are exactly 40 chars + NUL per zmq_curve_keypair.
constexpr std::size_t kZ85Len = 40;

auto SecretPath(const std::string &base,
                const std::string &name) -> std::string {
  return std::format("{}/{}.secret", base, name);
}

auto PublicPath(const std::string &base,
                const std::string &name) -> std::string {
  return std::format("{}/{}.public", base, name);
}

auto WriteTextFile(const std::string &path, const std::string &body,
                   mode_t mode) -> std::expected<void, Error<KeyError>> {
  try {
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
      return std::unexpected(MakeError(
          KeyError::IoFailed,
          std::format("could not open for write: {}", path)));
    }
    f << body;
    f.close();
    if (::chmod(path.c_str(), mode) != 0) {
      return std::unexpected(MakeError(
          KeyError::IoFailed,
          std::format("chmod failed on {}", path)));
    }
    return {};
  } catch (const std::exception &e) {
    return std::unexpected(MakeError(KeyError::IoFailed, e.what()));
  }
}

auto ReadTextFile(const std::string &path)
    -> std::expected<std::string, Error<KeyError>> {
  try {
    std::ifstream f(path);
    if (!f) {
      return std::unexpected(MakeError(
          KeyError::IoFailed,
          std::format("could not open for read: {}", path)));
    }
    std::string body;
    std::getline(f, body);
    return body;
  } catch (const std::exception &e) {
    return std::unexpected(MakeError(KeyError::IoFailed, e.what()));
  }
}

}  // namespace

auto Generate() -> std::expected<KeyPair, Error<KeyError>> {
  std::array<char, 41> pub{};
  std::array<char, 41> sec{};
  const int rc = zmq_curve_keypair(pub.data(), sec.data());
  if (rc != 0) {
    return std::unexpected(MakeError(
        KeyError::GenerationFailed, "zmq_curve_keypair failed"));
  }
  return KeyPair{pub.data(), sec.data()};
}

auto WriteToDisk(const std::string &base, const std::string &name,
                 const KeyPair &pair)
    -> std::expected<void, Error<KeyError>> {
  try {
    std::filesystem::create_directories(base);
  } catch (const std::exception &e) {
    return std::unexpected(MakeError(KeyError::BadDirectory, e.what()));
  }
  if (auto r = WriteTextFile(PublicPath(base, name),
                             pair.public_key, 0644);
      !r) {
    return std::unexpected(r.error());
  }
  if (auto r = WriteTextFile(SecretPath(base, name),
                             pair.secret_key, 0600);
      !r) {
    return std::unexpected(r.error());
  }
  return {};
}

auto ReadFromDisk(const std::string &base, const std::string &name)
    -> std::expected<KeyPair, Error<KeyError>> {
  auto pub = ReadTextFile(PublicPath(base, name));
  if (!pub) return std::unexpected(pub.error());
  auto sec = ReadTextFile(SecretPath(base, name));
  if (!sec) return std::unexpected(sec.error());
  if (pub->size() != kZ85Len || sec->size() != kZ85Len) {
    return std::unexpected(MakeError(
        KeyError::BadFormat,
        std::format("key length != {} (public {}, secret {})",
                    kZ85Len, pub->size(), sec->size())));
  }
  return KeyPair{*pub, *sec};
}

}  // namespace einheit::cli::curve
