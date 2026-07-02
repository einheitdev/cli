/// @file memory_backend.h
/// @brief MemoryBackend — a fake-hardware ConfigBackend for tests and
/// as the shipped reference product.
///
/// Apply() programs the candidate into an observable in-memory "device"
/// map and bumps a revision counter; ReadRunning() reads that device
/// state back. Tests assert on DeviceState() to prove apply / rollback
/// actually changed state — a stub that returned Ok without writing
/// would fail those assertions. Fault injection (FailNextApply) lets
/// tests exercise the failure paths (commit that doesn't advance
/// running, etc.).
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_MEMORY_BACKEND_H_
#define INCLUDE_EINHEIT_CLI_CONFD_MEMORY_BACKEND_H_

#include <expected>
#include <memory>
#include <mutex>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/error.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {

/// In-memory ConfigBackend simulating a programmable box.
class MemoryBackend : public ConfigBackend {
 public:
  /// Construct over a schema. The schema must be non-null; a null
  /// pointer is replaced with an empty schema so Schema() never
  /// dangles (gap #5 invariant).
  /// @param schema Loaded config schema.
  explicit MemoryBackend(std::shared_ptr<const schema::Schema> schema);

  auto Apply(const Candidate &candidate)
      -> std::expected<CommitId, Error<ApplyError>> override;
  auto ReadRunning() -> Config override;
  auto Schema() const -> const schema::Schema & override;

  /// The currently-programmed device state — what the box actually
  /// holds. Behavioural tests assert against this.
  auto DeviceState() const -> Config;

  /// How many times Apply has run (successful applies only).
  auto ApplyCount() const -> int;

  /// Make the next Apply fail with HardwareRejected, without touching
  /// device state. One-shot: cleared after it fires.
  auto FailNextApply() -> void;

 private:
  // Non-null by construction — a MemoryBackend built with a null
  // schema falls back to the empty DefaultSchema(), never a null deref.
  schema::SchemaHandle schema_;
  mutable std::mutex mu_;
  Config device_;
  CommitId rev_ = 0;
  int apply_count_ = 0;
  bool fail_next_ = false;
};

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_MEMORY_BACKEND_H_
