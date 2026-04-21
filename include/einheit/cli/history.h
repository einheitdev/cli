/// @file history.h
/// @brief Per-user persistent command history. Backed by a plain
/// text file at /var/lib/einheit/users/<user>/history.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_HISTORY_H_
#define INCLUDE_EINHEIT_CLI_HISTORY_H_

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "einheit/cli/error.h"

namespace einheit::cli {

/// Errors raised by the history module.
enum class HistoryError {
  /// Target directory did not exist or was not writable.
  NotAccessible,
  /// Corrupt or truncated history file.
  Corrupt,
  /// Filesystem operation failed.
  IoFailed,
};

/// The on-disk + in-memory shape of a user's history.
struct History {
  /// Absolute path to the backing file.
  std::string path;
  /// In-memory entries; appended on each accepted command.
  std::vector<std::string> entries;
  /// Max number of entries retained; older entries rotate out.
  std::uint32_t max_entries = 1000;
};

/// Default base directory for per-user history files.
inline constexpr const char *kDefaultHistoryBase =
    "/var/lib/einheit/users";

/// Load (or initialise) a user's history file.
/// @param user einheit user name.
/// @param base_path Directory prefix; the backing file lives at
/// `<base_path>/<user>/history`.
/// @returns Populated History or HistoryError.
auto Load(const std::string &user,
          const std::string &base_path = kDefaultHistoryBase)
    -> std::expected<History, Error<HistoryError>>;

/// Append one entry and flush to disk.
/// @param h History handle.
/// @param entry Command line to record.
auto Append(History &h, const std::string &entry)
    -> std::expected<void, Error<HistoryError>>;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_HISTORY_H_
