/// @file durable_write.h
/// @brief Atomic AND durable file replacement.
///
/// A temp-file-plus-rename is atomic against a crash: a reader never
/// sees a torn file. It is NOT durable against power loss — the data
/// and the rename can both still be in the page cache when the power
/// goes, and the file reverts to its previous contents on the next
/// boot. For a config store that is the difference between "commit
/// succeeded" and a lie: the operator saw the commit land, the box
/// booted the one before it.
///
/// Found by the Phase 0 power-cut soak (test/power_cycle.sh in s5): a
/// commit taken seconds before `virsh destroy` was gone after the
/// reboot, and the box restored the previous revision.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_DURABLE_WRITE_H_
#define INCLUDE_EINHEIT_CLI_CONFD_DURABLE_WRITE_H_

#include <expected>
#include <string>

#include "einheit/cli/error.h"

namespace einheit::cli::confd {

/// Why a durable write failed.
enum class DurableWriteError {
  /// The directory could not be created.
  DirFailed,
  /// The temp file could not be created or written.
  WriteFailed,
  /// The data could not be flushed to stable storage.
  SyncFailed,
  /// The rename into place failed.
  RenameFailed,
};

/// Replace `path` with `content`, atomically and durably: write a temp
/// file beside it, fsync the temp file, rename it into place, then
/// fsync the containing directory so the rename itself survives power
/// loss. Creates the parent directory if needed. On any failure the
/// temp file is removed and `path` keeps its previous contents.
///
/// Costs two fsyncs, so it belongs on state that must survive a power
/// cut (running config, commit history, saved configurations) and not
/// on throwaway state (a lock file whose whole point is to disappear).
/// @param path Destination file.
/// @param content Bytes to write.
/// @returns void on success, or a DurableWriteError.
auto WriteFileDurably(const std::string &path, const std::string &content)
    -> std::expected<void, Error<DurableWriteError>>;

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_DURABLE_WRITE_H_
