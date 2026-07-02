/// @file config_backend.h
/// @brief ConfigBackend — the daemon-side contract a product plugs
/// into, the mirror of the CLI-side ProductAdapter.
///
/// The CLI side has a clean seam (`ProductAdapter`): the framework owns
/// shell / transport / dispatch / render, the product plugs in. The
/// daemon side needs its counterpart so a product can plug in "apply
/// this committed candidate to the actual box". confd owns the config
/// lifecycle (candidate / commit / history / confirm / rollback); the
/// backend owns the actual apply to hardware. This is also where gap #4
/// is closed: config application lives here, never in the const,
/// display-only render surface.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_CONFIG_BACKEND_H_
#define INCLUDE_EINHEIT_CLI_CONFD_CONFIG_BACKEND_H_

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>

#include "einheit/cli/error.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {

/// A config is a flat map of dotted schema-path → string value. Flat
/// matches the schema's dotted-path validation and is enough to drive
/// the commit machinery; a product with a richer internal model
/// translates in its ConfigBackend::Apply.
using Config = std::unordered_map<std::string, std::string>;

/// The candidate configuration being edited inside a configure
/// session, or the frozen snapshot recorded by a commit.
struct Candidate {
  /// Flat path → value map.
  Config values;
};

/// Monotonic commit revision id assigned by the backend on Apply.
/// It is the canonical "what is running" handle both confd and the
/// product daemon agree on (for the firewall, e.g. the fd bundle id).
/// Revision 0 means "no commit yet".
using CommitId = std::uint64_t;

/// Why a ConfigBackend::Apply failed.
enum class ApplyError {
  /// Candidate failed schema/semantic validation before touching HW.
  ValidationFailed,
  /// The box rejected the config; running state is unchanged.
  HardwareRejected,
  /// Apply started but could not complete; the box may be
  /// inconsistent and the caller should treat this as fatal.
  PartialApply,
  /// Backend is not ready to accept an apply.
  Unavailable,
};

/// The product implements this. confd's runtime calls Apply on commit
/// and when re-applying a prior revision on rollback / auto-revert;
/// Apply is the single mutation seam to the real box.
class ConfigBackend {
 public:
  virtual ~ConfigBackend() = default;

  /// Apply a candidate to the real box. Returns the new commit id on
  /// success. On error the runtime does NOT advance its running state,
  /// so a failed commit or rollback leaves the last-good config live.
  /// @param candidate The configuration to program onto the box.
  /// @returns The new CommitId, or an ApplyError.
  virtual auto Apply(const Candidate &candidate)
      -> std::expected<CommitId, Error<ApplyError>> = 0;

  /// Read the live running configuration back from the box/system.
  /// confd uses this to reconcile its notion of running with reality
  /// (e.g. at startup, before the first commit).
  /// @returns The current running config.
  virtual auto ReadRunning() -> Config = 0;

  /// The config schema — single source of truth for validation and
  /// completion. Must return a stable, non-null reference for the
  /// backend's lifetime (the framework relies on no maybe-null deref).
  /// @returns The loaded schema.
  virtual auto Schema() const -> const schema::Schema & = 0;
};

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_CONFIG_BACKEND_H_
