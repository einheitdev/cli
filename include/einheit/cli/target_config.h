/// @file target_config.h
/// @brief Workstation-side target configuration (`~/.einheit/config`).
///
/// Operators maintain a list of appliances they manage remotely. The
/// CLI picks one via `einheit --target <name>` or `einheit use
/// <name>`. Each target carries the endpoint + pinned server Curve
/// key + path to the client secret key. This module parses the YAML
/// and resolves a named target.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_TARGET_CONFIG_H_
#define INCLUDE_EINHEIT_CLI_TARGET_CONFIG_H_

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "einheit/cli/error.h"

namespace einheit::cli::target {

/// Errors raised by target config loading / resolution.
enum class TargetError {
  /// Config file does not exist or is not readable.
  NotFound,
  /// YAML is malformed.
  ParseFailed,
  /// A target entry is missing a required field.
  InvalidTarget,
  /// `--target <name>` was specified but didn't match any entry.
  UnknownTarget,
  /// Neither an explicit target nor a `default:` was provided.
  NoDefault,
};

/// One appliance the workstation can address.
struct Target {
  /// Short name shown in UI and used with --target.
  std::string name;
  /// tcp://host:port of the appliance's ROUTER control socket.
  std::string control_endpoint;
  /// tcp://host:port of the appliance's XPUB event socket. When
  /// empty the control_endpoint's port+1 is assumed.
  std::string event_endpoint;
  /// Z85-encoded Curve public key of the appliance. Pinned by the
  /// client; the handshake fails if the daemon presents any other.
  std::string server_public_key;
  /// Filesystem path to the client's Curve secret key (mode 0600).
  std::string client_secret_key_path;
};

/// Parsed contents of `~/.einheit/config`.
struct TargetConfig {
  /// All known targets, keyed by name in insertion order.
  std::vector<Target> targets;
  /// Name of the entry used when the user doesn't pass --target.
  std::optional<std::string> default_target;
};

/// Load a target config from an explicit path.
/// @param path Path to the YAML file.
/// @returns Parsed config or TargetError.
auto LoadFromFile(const std::string &path)
    -> std::expected<TargetConfig, Error<TargetError>>;

/// Load from the standard location (`$HOME/.einheit/config`).
/// @returns Parsed config or TargetError.
auto LoadFromHome()
    -> std::expected<TargetConfig, Error<TargetError>>;

/// Pick a target out of a loaded config. When `name` is empty, use
/// `default_target`; otherwise look up by name.
/// @param cfg Loaded config.
/// @param name Explicit target name; empty to pick the default.
/// @returns Pointer to the matching target (owned by `cfg`).
auto Resolve(const TargetConfig &cfg, const std::string &name)
    -> std::expected<const Target *, Error<TargetError>>;

}  // namespace einheit::cli::target

#endif  // INCLUDE_EINHEIT_CLI_TARGET_CONFIG_H_
