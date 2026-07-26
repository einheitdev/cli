/// @file boot_report.h
/// @brief What the last boot-restore did, persisted for the CLI.
///
/// Boot APPLY needs no operator verb — `rollback to <id>` re-applies a
/// commit and is the operator-facing equivalent. The boot OUTCOME is a
/// different thing: it is state, and the CLI-completeness rule says
/// every piece of state has a `show` verb. Without one, "the box came
/// up wrong" has no answer short of reading the journal.
///
/// The report carries the kernel's boot id, and that is the load-
/// bearing field rather than a detail. A boot where the boot-restore
/// unit never ran leaves the PREVIOUS boot's report in place, which
/// would otherwise read as a healthy boot; comparing the recorded boot
/// id against the live one turns that silent case into an explicit
/// "boot-restore did not run on this boot". That failure is not
/// hypothetical — a systemd ordering cycle deleted the boot job on 2 of
/// 50 power cuts during Phase 0, and the box came up unconfigured with
/// nothing anywhere saying so.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_BOOT_REPORT_H_
#define INCLUDE_EINHEIT_CLI_CONFD_BOOT_REPORT_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "einheit/cli/confd/config_backend.h"
#include "einheit/cli/error.h"

namespace einheit::cli::confd {

/// One subsystem's contribution to a boot, in execution order. Products
/// add their own (an s5 records its switch-fabric bootstrap); the
/// runtime adds the configuration apply.
struct BootStep {
  /// Subsystem name, e.g. "fabric" or "config-apply".
  std::string name;
  /// Whether the step succeeded.
  bool ok = true;
  /// Human-readable outcome; the error message when !ok.
  std::string detail;
  /// Wall time spent in this step.
  std::int64_t duration_ms = 0;
};

/// The outcome of one boot-time apply.
struct BootReport {
  /// Kernel boot id (/proc/sys/kernel/random/boot_id) at the time of
  /// writing. Differs from the live id exactly when this report belongs
  /// to an earlier boot.
  std::string boot_id;
  /// RFC 3339 timestamp of the boot apply.
  std::string timestamp;
  /// Whether every step succeeded.
  bool ok = true;
  /// Commit whose configuration was applied; 0 when none was.
  CommitId applied_revision = 0;
  /// Config paths in the applied configuration.
  std::size_t paths = 0;
  /// The shipped factory configuration was seeded as the first commit.
  bool seeded_factory = false;
  /// An unconfirmed commit-confirmed window was reverted at boot.
  bool reverted_pending = false;
  /// Paths the box reported that the applied configuration did not
  /// carry. Routine — the box always knows things config does not say.
  std::size_t reconcile_added = 0;
  /// Paths where the box disagreed with the value just applied to it.
  /// This is the out-of-band-change signal: nonzero means reality did
  /// not accept, or has already drifted from, committed intent.
  std::size_t reconcile_conflicts = 0;
  /// Total wall time of the boot apply.
  std::int64_t duration_ms = 0;
  /// Per-subsystem detail.
  std::vector<BootStep> steps;
};

/// Errors raised by the boot-report store.
enum class BootReportError {
  /// Could not write the report.
  WriteFailed,
  /// Could not read an existing report.
  ReadFailed,
  /// The report file was present but malformed.
  ParseFailed,
};

/// The running kernel's boot id, or empty when it cannot be read (a
/// kernel without the attribute, or a sandbox). An empty id disables
/// the this-boot comparison rather than faking one.
/// @returns The boot id, or an empty string.
auto CurrentBootId() -> std::string;

/// Whether `report` was written by the currently running boot. A report
/// with no boot id, or an unreadable live id, answers false — "cannot
/// prove it ran this boot" is the safe reading.
/// @param report Report to test.
/// @param current_boot_id Live boot id, from CurrentBootId().
/// @returns Whether the report belongs to this boot.
auto IsFromCurrentBoot(const BootReport &report,
                       const std::string &current_boot_id) -> bool;

/// Load the last boot report from `dir`. A missing file is NOT an error
/// — it returns nullopt, meaning no boot apply has ever run here.
/// @param dir State directory.
/// @returns The report, nullopt when absent, or a BootReportError.
auto LoadBootReport(const std::string &dir)
    -> std::expected<std::optional<BootReport>, Error<BootReportError>>;

/// Persist `report` to `dir`, atomically and durably — a report that a
/// power cut can roll back would describe the wrong boot.
/// @param dir State directory.
/// @param report Report to persist.
/// @returns void, or a BootReportError.
auto SaveBootReport(const std::string &dir, const BootReport &report)
    -> std::expected<void, Error<BootReportError>>;

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_BOOT_REPORT_H_
