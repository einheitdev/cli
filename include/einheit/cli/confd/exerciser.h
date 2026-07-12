/// @file exerciser.h
/// @brief Schema-driven config exerciser — coverage by construction.
///
/// The schema is the single source of truth for what "the whole
/// config surface" means, so the test surface is derived from it
/// instead of hand-enumerated: for every leaf the generator emits
/// valid, boundary, and per-type invalid values, and the driver
/// pushes each through the full lifecycle (set → diff → commit →
/// verify → rollback) against a Runtime. New schema paths are
/// covered the moment they exist. Runs against MemoryBackend in
/// unit tests and against a product's real backend on a disposable
/// target.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_CONFD_EXERCISER_H_
#define INCLUDE_EINHEIT_CLI_CONFD_EXERCISER_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {

/// One generated probe: set `path` to `value`, expecting the
/// lifecycle to accept (valid) or reject without side effects.
struct ExerciseCase {
  /// Dotted config path (map keys already substituted).
  std::string path;
  /// Raw value token, as an operator would type it.
  std::string value;
  /// Whether the runtime must accept this value.
  bool valid = true;
  /// Human-readable case label ("range max", "bad enum", ...).
  std::string note;
};

/// One case the runtime handled incorrectly.
struct ExerciseFailure {
  ExerciseCase c;
  /// What went wrong, with the observed response/state.
  std::string detail;
};

/// Generation knobs.
struct ExerciseOptions {
  /// Concrete key to use per map node (the schema cannot enumerate
  /// map keys — an interface name or port number is the product's
  /// to provide). Keyed by the map node's dotted path ("ports");
  /// absent entries default to "1" for integer keys, "k1" for
  /// string keys.
  std::unordered_map<std::string, std::string> map_keys;
  /// Dotted path prefixes to skip entirely (hardware absent on the
  /// test target, e.g. "poe" on a PoE-less box).
  std::vector<std::string> skip_prefixes;
};

/// Walk `schema` and generate the full case set.
auto GenerateCases(const schema::Schema &schema,
                   const ExerciseOptions &opts = {})
    -> std::vector<ExerciseCase>;

/// Drive every case through `rt`: valid values must set, appear in
/// `show diff`, commit, land in Running() without touching any
/// other path, and be restored exactly by `rollback previous`;
/// invalid values must be rejected (at set or commit) leaving
/// Running() untouched.
/// @returns The failing cases with details; empty means the whole
///   surface passed.
auto ExerciseRuntime(Runtime &rt,
                     const std::vector<ExerciseCase> &cases)
    -> std::vector<ExerciseFailure>;

}  // namespace einheit::cli::confd

#endif  // INCLUDE_EINHEIT_CLI_CONFD_EXERCISER_H_
