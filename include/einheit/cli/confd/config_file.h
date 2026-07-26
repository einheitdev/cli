/// @file config_file.h
/// @brief Portable config files — the backup / restore / factory-reset
/// surface.
///
/// A saved configuration is one flat `path value` file: the same shape
/// the store already writes for running config, minus the tags. That
/// makes a backup diffable, hand-editable, and trivially portable
/// between boxes of the same product.
///
/// Files are addressed by *name*, never by path. The CLI runs as root
/// on the box, so accepting an operator-supplied path would hand out
/// arbitrary root reads and writes; a name is validated to a single
/// filesystem component and resolved inside one directory instead.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_CONFIG_FILE_H_
#define INCLUDE_EINHEIT_CLI_CONFD_CONFIG_FILE_H_

#include <expected>
#include <string>
#include <vector>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/error.h"

namespace einheit::cli::confd {

/// First line of every file this module writes. Read back as a version
/// marker so a future format change can be detected rather than
/// misparsed.
inline constexpr const char *kConfigFileHeader = "# einheit-config v1";

/// Extension appended to a config name on disk.
inline constexpr const char *kConfigFileSuffix = ".conf";

/// Why a config-file operation failed.
enum class ConfigFileError {
  /// The name is not a single safe filesystem component.
  BadName,
  /// No such config file.
  NotFound,
  /// Could not read the file.
  ReadFailed,
  /// Could not write the file.
  WriteFailed,
  /// The file is not a valid flat-kv config.
  ParseFailed,
  /// No config directory is configured (in-memory-only runtime).
  NoConfigDir,
};

/// True when `name` is a safe config-file name: 1-64 characters of
/// [A-Za-z0-9._-], not starting with '.', and containing no path
/// separator. Deliberately strict — this is the guard that keeps
/// `load replace ../../etc/shadow` from being a thing.
/// @param name Operator-supplied config name.
/// @returns Whether the name may be resolved to a file.
auto ValidConfigName(const std::string &name) -> bool;

/// Resolve `name` to a path inside `dir`.
/// @param dir Config directory.
/// @param name Config name (validated).
/// @returns The full path, or a ConfigFileError.
auto ConfigFilePath(const std::string &dir, const std::string &name)
    -> std::expected<std::string, Error<ConfigFileError>>;

/// Write `config` to `path` atomically (temp + rename), creating the
/// parent directory. Keys are emitted in sorted order so two saves of
/// the same configuration produce byte-identical files.
/// @param path Destination file.
/// @param config Configuration to serialize.
/// @returns void on success, or a ConfigFileError.
auto WriteConfigFile(const std::string &path, const Config &config)
    -> std::expected<void, Error<ConfigFileError>>;

/// Read a flat-kv config file. Blank lines and `#` comments are
/// skipped; every other line must be `path value`.
/// @param path Source file.
/// @returns The parsed configuration, or a ConfigFileError.
auto ReadConfigFile(const std::string &path)
    -> std::expected<Config, Error<ConfigFileError>>;

/// Names of the config files in `dir`, sorted. A missing directory
/// yields an empty list rather than an error.
/// @param dir Config directory.
/// @returns Saved config names, without the file suffix.
auto ListConfigFiles(const std::string &dir) -> std::vector<std::string>;

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_CONFIG_FILE_H_
