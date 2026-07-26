/// @file edit_lock.h
/// @brief Durable configure-mode edit lock.
///
/// MANAGEMENT_PLANE.md requires that a second editor be refused
/// rather than allowed to clobber the candidate. An in-process flag
/// is not enough: a product like s5 embeds its confd Runtime in the
/// CLI binary, so two logged-in operators are two processes with two
/// Runtimes. The lock therefore lives in a file under the state
/// directory, where every process that shares the durable store can
/// see it.
///
/// "The lock dies with the session" is implemented as pid liveness:
/// a lock whose owning process is gone is stale and the next
/// `configure` reclaims it silently. A live holder can only be
/// displaced by an explicit `configure force` (admin).
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_EDIT_LOCK_H_
#define INCLUDE_EINHEIT_CLI_CONFD_EDIT_LOCK_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "einheit/cli/error.h"

namespace einheit::cli::confd {

/// Who currently holds configure mode.
struct EditLock {
  /// User who opened the configure session.
  std::string holder;
  /// Runtime-issued session id (unique across processes).
  std::string session_id;
  /// Process that owns the session; 0 when unknown.
  std::int64_t pid = 0;
  /// RFC 3339 timestamp of acquisition.
  std::string acquired_at;
};

/// Why an edit-lock operation failed.
enum class LockError {
  /// Another live session holds configure mode.
  Held,
  /// Could not write the lock file.
  WriteFailed,
  /// Could not read an existing lock file.
  ReadFailed,
  /// Lock file was present but malformed.
  ParseFailed,
};

/// What acquiring the lock displaced, if anything.
struct AcquireOutcome {
  /// Live holder displaced by `force`.
  std::optional<EditLock> stolen_from;
  /// Holder whose process had died, whose stale lock was reclaimed.
  std::optional<EditLock> reclaimed_from;
};

/// True when the process named by `lock` still exists. A lock with an
/// unknown pid (0) counts as alive: refusing is the safe answer when
/// we cannot tell.
/// @param lock Lock to test.
/// @returns Whether the holder's process is still around.
auto EditLockHolderAlive(const EditLock &lock) -> bool;

/// Read the current lock. A missing file is NOT an error — it returns
/// nullopt, meaning configure mode is free.
/// @param dir State directory.
/// @returns The lock, nullopt when free, or a LockError.
auto ReadEditLock(const std::string &dir)
    -> std::expected<std::optional<EditLock>, Error<LockError>>;

/// Take configure mode for `want`. Fails with LockError::Held when a
/// live holder has it and `force` is false; a stale lock (dead pid) is
/// reclaimed without force. Re-acquiring one's own session id is a
/// no-op refresh.
/// @param dir State directory.
/// @param want Identity to record as the new holder.
/// @param force Displace a live holder (`configure force`).
/// @returns What the acquisition displaced, or a LockError.
auto AcquireEditLock(const std::string &dir, const EditLock &want,
                     bool force)
    -> std::expected<AcquireOutcome, Error<LockError>>;

/// Drop the lock if `session_id` still holds it. A no-op when the lock
/// is free or held by someone else (a session that was force-stolen
/// must not release the thief's lock on its way out). Best-effort:
/// releasing never fails a caller.
/// @param dir State directory.
/// @param session_id Session releasing the lock.
auto ReleaseEditLock(const std::string &dir,
                     const std::string &session_id) -> void;

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_EDIT_LOCK_H_
