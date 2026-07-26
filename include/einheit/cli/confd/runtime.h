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
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "einheit/cli/audit.h"
#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/confd/edit_lock.h"
#include "einheit/cli/confd/store.h"
#include "einheit/cli/error.h"
#include "einheit/cli/protocol/envelope.h"

namespace einheit::cli::confd {

/// Construction-time options for a Runtime.
struct RuntimeOptions {
  /// Authoritative daemon-side audit sink; one Record per mutating
  /// request. May be empty (no-op).
  audit::Sink audit;
  /// Directory for durable state (running config + history + pending
  /// commit-confirm + the configure-mode edit lock). Empty means
  /// in-memory only: no persistence, and the edit lock degrades to a
  /// process-local flag.
  std::string state_dir;
  /// Directory holding operator-saved configuration files (`save`,
  /// `load merge|replace`). Empty means `<state_dir>/configs`; empty
  /// with no state_dir disables the config-file surface.
  std::string config_dir;
  /// Path to the product's shipped factory-defaults config file, used
  /// by `load factory`. Empty (or missing on disk) means the factory
  /// configuration is the empty configuration.
  std::string factory_config;
};

/// What a boot-time apply did.
struct BootApplyResult {
  /// True when a committed configuration was pushed to the box.
  bool applied = false;
  /// Number of config paths in the applied configuration.
  std::size_t paths = 0;
  /// Commit id whose configuration was applied; 0 when none was.
  CommitId commit = 0;
  /// True when an unconfirmed commit-confirmed window was resolved by
  /// reverting before the apply.
  bool reverted_pending = false;
  /// True when there was no commit history and the shipped factory
  /// configuration was applied and recorded as the first commit.
  bool seeded_factory = false;
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

  /// Re-apply the committed configuration to the box at boot, then
  /// reconcile. Every s1xx product needs this: nothing else makes a
  /// box wake up in the configuration its operator committed — without
  /// it the runtime adopts whatever state the hardware powered on
  /// with, and a reboot silently means factory behaviour.
  ///
  /// The target is the last commit's candidate, not the reconciled
  /// running config: reality-wins reconciliation is right for a
  /// long-lived daemon that must not fight the box, and exactly wrong
  /// here, where the box's power-on defaults are what we are
  /// overwriting. After the apply, intent wins for every path the
  /// commit carries and the box fills in the rest.
  ///
  /// A commit-confirmed window that is still armed is resolved by
  /// reverting first: boot is the deadline. The boot apply is a
  /// oneshot with no live timer to fire the auto-revert later, so
  /// carrying the window forward would silently make an unconfirmed
  /// configuration permanent — the one outcome commit-confirmed
  /// exists to prevent.
  ///
  /// Intended to be called once, before any front-end is reachable.
  /// With no commit history the shipped factory configuration is
  /// applied and recorded as the first commit — an unconfigured box is
  /// not a neutral state, since a switch whose ports were never
  /// enabled forwards nothing. With no factory file either, the call
  /// succeeds having done nothing.
  /// @returns What was applied, or the backend's ApplyError.
  auto ApplyRunningAtBoot()
      -> std::expected<BootApplyResult, Error<ApplyError>>;

  /// Current running configuration (post last successful commit).
  /// Exposed for status and tests.
  auto Running() const -> Config;

  /// Who holds configure mode, or nullopt when it is free. Reads the
  /// durable lock when the runtime has a state dir, so it also sees a
  /// session held by another process.
  auto EditLockState() const -> std::optional<EditLock>;

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
