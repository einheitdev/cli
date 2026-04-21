/// @file curve_keys.h
/// @brief CurveZMQ keypair generation + on-disk serialisation.
///
/// Wraps `zmq_curve_keypair()` and writes pairs to disk in the layout
/// used by the target config: secret key at `<base>/<name>.secret`
/// (mode 0600), public key at `<base>/<name>.public` (mode 0644).
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CURVE_KEYS_H_
#define INCLUDE_EINHEIT_CLI_CURVE_KEYS_H_

#include <expected>
#include <string>

#include "einheit/cli/error.h"

namespace einheit::cli::curve {

/// Errors raised by keypair generation or serialisation.
enum class KeyError {
  /// Underlying zmq_curve_keypair() failed.
  GenerationFailed,
  /// Filesystem write failed.
  IoFailed,
  /// Z85 blob on disk did not decode to a 32-byte key.
  BadFormat,
  /// Target directory did not exist and couldn't be created.
  BadDirectory,
};

/// One Curve25519 keypair in Z85 string form (40 characters each).
struct KeyPair {
  std::string public_key;
  std::string secret_key;
};

/// Generate a fresh Curve25519 keypair.
/// @returns Generated KeyPair or KeyError::GenerationFailed.
auto Generate() -> std::expected<KeyPair, Error<KeyError>>;

/// Write a keypair to `<base>/<name>.public` and
/// `<base>/<name>.secret`. The secret file is created with mode 0600.
/// @param base Directory to write into. Created if missing.
/// @param name Filename stem (no extension).
/// @param pair Keys to write.
/// @returns void on success, KeyError otherwise.
auto WriteToDisk(const std::string &base, const std::string &name,
                 const KeyPair &pair)
    -> std::expected<void, Error<KeyError>>;

/// Read a previously-written keypair back from disk.
/// @param base Directory containing the keys.
/// @param name Filename stem.
/// @returns Loaded pair or KeyError.
auto ReadFromDisk(const std::string &base, const std::string &name)
    -> std::expected<KeyPair, Error<KeyError>>;

}  // namespace einheit::cli::curve

#endif  // INCLUDE_EINHEIT_CLI_CURVE_KEYS_H_
