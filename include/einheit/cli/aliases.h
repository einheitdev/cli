/// @file aliases.h
/// @brief Per-user command aliases. Loaded on shell start; expanded
/// before parse.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_ALIASES_H_
#define INCLUDE_EINHEIT_CLI_ALIASES_H_

#include <expected>
#include <string>
#include <unordered_map>

#include "einheit/cli/error.h"

namespace einheit::cli {

/// Errors raised by the aliases module.
enum class AliasError {
  /// Backing file not found or not readable.
  NotAccessible,
  /// Alias file has malformed syntax.
  Malformed,
  /// Tried to register an alias that would cycle.
  Cycle,
};

/// The on-disk + in-memory alias table.
struct Aliases {
  /// Absolute path to the backing file.
  std::string path;
  /// Alias name -> expanded command string.
  std::unordered_map<std::string, std::string> table;
  /// Optional per-alias help text surfaced by `alias`. Missing
  /// entries mean "no description".
  std::unordered_map<std::string, std::string> help;
};

/// Default base directory for per-user alias files.
inline constexpr const char *kDefaultAliasBase =
    "/var/lib/einheit/users";

/// Load the user's alias table from disk using the legacy
/// `name=expansion` line format.
/// @param user einheit user name.
/// @param base_path Directory prefix; the backing file lives at
/// `<base_path>/<user>/aliases`.
/// @returns Populated Aliases or AliasError.
auto LoadAliases(const std::string &user,
                 const std::string &base_path = kDefaultAliasBase)
    -> std::expected<Aliases, Error<AliasError>>;

/// Load an alias file in the YAML format shared by team members.
/// Supports two shapes for each entry:
///
///   aliases:
///     st: show tunnels                       # string form
///     sc:                                    # structured form
///       expansion: show config
///       help: "Dump the running config"
///
///   include:                                 # merged first-to-last;
///     - /etc/einheit/team-aliases.yaml       # later entries win
///
/// @param path Absolute path to the YAML file.
/// @returns Populated Aliases or AliasError.
auto LoadAliasesYaml(const std::string &path)
    -> std::expected<Aliases, Error<AliasError>>;

/// Default YAML alias path under the user's home directory.
/// @returns Absolute path `$HOME/.einheit/aliases.yaml`, empty if
/// `$HOME` is unset.
auto DefaultYamlPath() -> std::string;

/// Merge `other` into `base` in place. Entries in `other` win on
/// conflict; help text is merged the same way.
/// @param base Target.
/// @param other Source; unchanged.
auto MergeAliases(Aliases &base, const Aliases &other) -> void;

/// Add (or replace) one alias in a YAML file. Preserves other keys
/// in the document — notably `include:` — so this is safe to call
/// on a user file that composes several sources. Creates the file
/// (and its parent directory) if missing.
/// @param path Absolute path to the YAML file to mutate.
/// @param name Alias name (first token of the eventual line).
/// @param expansion The text the name expands to.
/// @returns void on success, AliasError otherwise.
auto SetYamlAlias(const std::string &path, const std::string &name,
                  const std::string &expansion)
    -> std::expected<void, Error<AliasError>>;

/// Remove an alias from the YAML file. No-op if the name isn't
/// present. Returns AliasError::NotAccessible if the file itself
/// is missing.
/// @param path Absolute path to the YAML file to mutate.
/// @param name Alias to remove.
/// @returns void on success, AliasError otherwise.
auto RemoveYamlAlias(const std::string &path, const std::string &name)
    -> std::expected<void, Error<AliasError>>;

/// Expand leading alias on an input line. No-op if first token is
/// not a known alias. Cycle-safe: expansion is single-pass.
/// @param a Loaded alias table.
/// @param line Raw input line.
/// @returns Expanded line (may be identical to input).
auto Expand(const Aliases &a, const std::string &line) -> std::string;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_ALIASES_H_
