/// @file runtime.h
/// @brief confd daemon runtime — the framework-owned config lifecycle.
///
/// This is to the daemon what RunShell is to the CLI: built once in the
/// framework, not per product. It owns session ids, candidate
/// accumulation from set/delete, commit → ConfigBackend::Apply,
/// versioned history, and rollback. The core is transport-agnostic:
/// HandleRequest(Request) → Response is driven identically by the ZMQ
/// server (standalone, multi-process) and the in-process transport
/// (embedded single binary) — one code path, not two. It supersedes
/// learning_daemon, which stays only as a lightweight test double.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_RUNTIME_H_
#define INCLUDE_EINHEIT_CLI_CONFD_RUNTIME_H_

#include <cstddef>
#include <memory>
#include <string>

#include "einheit/cli/audit.h"
#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/confd/store.h"
#include "einheit/cli/protocol/envelope.h"

namespace einheit::cli::confd {

/// Construction-time options for a Runtime.
struct RuntimeOptions {
  /// Authoritative daemon-side audit sink; one Record per mutating
  /// request. May be empty (no-op).
  audit::Sink audit;
  /// Directory for durable state (running config + history + pending
  /// commit-confirm). Empty means in-memory only (no persistence).
  std::string state_dir;
};

/// The framework-owned config lifecycle runtime. Thread-safe: every
/// public method takes the internal lock, so a ZMQ server thread and
/// an in-process caller can share one Runtime.
class Runtime {
 public:
  /// Construct a runtime over a product backend. The backend must
  /// outlive the runtime. Seeds running config from
  /// ConfigBackend::ReadRunning().
  /// @param backend Product apply seam (borrowed).
  /// @param opts Optional audit sink and future knobs.
  explicit Runtime(ConfigBackend &backend, RuntimeOptions opts = {});
  ~Runtime();

  Runtime(const Runtime &) = delete;
  auto operator=(const Runtime &) -> Runtime & = delete;

  /// Handle one decoded Request and produce a Response. The single,
  /// transport-agnostic entry point every front-end reaches through.
  /// @param req Decoded wire request.
  /// @returns The response to send back.
  auto HandleRequest(const protocol::Request &req) -> protocol::Response;

  /// Current running configuration (post last successful commit).
  /// Exposed for status and tests.
  auto Running() const -> Config;

  /// Number of commits recorded in history.
  auto HistorySize() const -> std::size_t;

  /// Snapshot of the pending commit-confirm window (armed=false when
  /// none). Lets a front-end or test read the live countdown without
  /// going through the wire status command.
  auto PendingConfirmState() const -> PendingConfirm;

  // Opaque implementation, exposed only so the .cc file can define
  // the free-function helpers.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_RUNTIME_H_
