/// @file store.h
/// @brief Durable persistence for the confd runtime.
///
/// The framework-owned persistence primitive: running config + commit
/// history + revision counter + pending commit-confirmed state, written
/// atomically to a single file so it survives a confd restart AND a
/// reboot. `rollback previous` after a restart depends on this history
/// being durable. Values are single CLI tokens (no embedded newlines),
/// so a line-based format is both robust for this domain and
/// inspectable.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_STORE_H_
#define INCLUDE_EINHEIT_CLI_CONFD_STORE_H_

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/error.h"

namespace einheit::cli::confd {

/// One recorded commit: the applied candidate plus who/when, the
/// runtime-owned canonical revision id, and the backend's own handle.
struct CommitRecord {
  /// Runtime-owned canonical revision (monotonic, durable across a
  /// backend restart). This is the id users see and roll back to.
  CommitId id = 0;
  /// Handle the ConfigBackend returned from Apply (e.g. an fd bundle
  /// id) — kept so confd and the product daemon agree on what is live.
  CommitId backend_id = 0;
  /// The frozen candidate applied by this commit.
  Candidate candidate;
  /// User who authored the commit.
  std::string author;
  /// RFC 3339 timestamp of the commit.
  std::string timestamp;
};

/// Pending commit-confirmed state. Persisted so the auto-revert
/// survives a confd restart: on startup an expired deadline fires
/// immediately, a live one re-arms for the remaining window.
struct PendingConfirm {
  /// True when a commit-confirmed window is open.
  bool armed = false;
  /// Commit id to revert to if the window closes unconfirmed. 0 means
  /// revert to the empty config (the confirmed commit was the first).
  CommitId rollback_to = 0;
  /// Absolute deadline as epoch milliseconds (UTC). Milliseconds so a
  /// short window survives a restart without rounding to a whole
  /// second.
  std::int64_t deadline_epoch_ms = 0;
  /// The commit id that armed the window (the one being confirmed).
  CommitId pending_commit = 0;
};

/// The complete durable state of a runtime.
struct PersistentState {
  /// Running configuration after the last successful commit.
  Config running;
  /// Full commit history in chronological order.
  std::vector<CommitRecord> history;
  /// Next canonical revision to assign.
  CommitId next_rev = 0;
  /// Pending commit-confirmed window, if any.
  PendingConfirm pending;
};

/// Errors raised by the store.
enum class StoreError {
  /// Could not write the state file.
  WriteFailed,
  /// Could not read an existing state file.
  ReadFailed,
  /// State file was present but malformed.
  ParseFailed,
};

/// Load persisted state from `dir`. A missing directory or state file
/// is NOT an error — it returns default (empty) state, i.e. first boot.
/// @param dir State directory.
/// @returns Loaded (or empty) state, or a StoreError on a corrupt file.
auto LoadState(const std::string &dir)
    -> std::expected<PersistentState, Error<StoreError>>;

/// Persist `state` to `dir` atomically (write temp + rename), creating
/// the directory if needed.
/// @param dir State directory.
/// @param state State to persist.
/// @returns void on success, or a StoreError.
auto SaveState(const std::string &dir, const PersistentState &state)
    -> std::expected<void, Error<StoreError>>;

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_STORE_H_
