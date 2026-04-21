/// @file command_tree.h
/// @brief Command registry + dispatch. Adapters publish CommandSpecs;
/// the shell walks the registry to parse, complete, and dispatch.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_COMMAND_TREE_H_
#define INCLUDE_EINHEIT_CLI_COMMAND_TREE_H_

#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include "einheit/cli/error.h"

namespace einheit::cli {

namespace schema { struct Schema; }

/// Errors raised by command registration or dispatch.
enum class CommandTreeError {
  /// Spec conflicts with an existing registration.
  DuplicateRegistration,
  /// Input did not match any registered spec.
  UnknownCommand,
  /// Required argument missing.
  MissingArgument,
  /// Argument failed its type / range check.
  InvalidArgument,
  /// Caller's role is not authorised for this command.
  NotAuthorised,
};

/// Role gate on a command. Matches the three roles in the spec.
enum class RoleGate {
  /// Anyone authenticated may run the command.
  AnyAuthenticated,
  /// Requires operator or admin role.
  OperatorOrAdmin,
  /// Requires admin role only.
  AdminOnly,
};

/// One positional argument in a CommandSpec.
struct ArgSpec {
  /// Argument display name ("tunnel_name").
  std::string name;
  /// Human-readable help shown by `?`.
  std::string help;
  /// Whether the argument is required at parse time.
  bool required = true;
  /// Optional schema type reference for validation/completion. Empty
  /// means free-form string.
  std::string type_ref;
};

/// Declarative description of one verb/noun command.
struct CommandSpec {
  /// Space-separated path, e.g. "show tunnels" or "test tunnel".
  /// First token is the verb; subsequent tokens are nouns.
  std::string path;
  /// Positional arguments after the path.
  std::vector<ArgSpec> args;
  /// Named flags (--format, --color, --target). Values are free-form.
  std::vector<std::string> flags;
  /// Role required to invoke this command.
  RoleGate role = RoleGate::AnyAuthenticated;
  /// Wire command string carried in Request::command.
  std::string wire_command;
  /// Help paragraph for `help <command>`.
  std::string help;
  /// True iff this command needs a candidate-config session.
  bool requires_session = false;
};

/// Registry of all commands known to this CLI invocation. Populated
/// at startup: framework adds global verbs, adapter adds product
/// verbs.
struct CommandTree {
  /// All registered specs keyed by their path.
  std::unordered_map<std::string, CommandSpec> by_path;
};

/// Result of a successful parse: which spec matched plus the
/// extracted positional args and flags. Ready for wire encoding.
struct ParsedCommand {
  /// The spec that matched the input.
  const CommandSpec *spec = nullptr;
  /// Positional values in the order declared on the spec.
  std::vector<std::string> args;
  /// Flag values keyed by flag name (no leading dashes).
  std::unordered_map<std::string, std::string> flags;
};

/// Register one command. Called during startup by framework + adapter.
/// @param tree Registry being built.
/// @param spec Command to register. Copied.
/// @returns void on success or CommandTreeError on duplicate.
auto Register(CommandTree &tree, CommandSpec spec)
    -> std::expected<void, Error<CommandTreeError>>;

/// Parse a whitespace-split command line into a ParsedCommand. Does
/// not validate schema-typed args; does check presence + role.
/// @param tree Registered commands.
/// @param tokens Input tokens (whitespace-split, no leading verb
/// normalisation).
/// @param caller_role Role of the user running the command.
/// @returns ParsedCommand on success, CommandTreeError otherwise.
auto Parse(const CommandTree &tree,
           const std::vector<std::string> &tokens,
           RoleGate caller_role)
    -> std::expected<ParsedCommand, Error<CommandTreeError>>;

/// Suggest completions for the current token given preceding tokens.
/// Pure command-tree view: walks the registered paths.
/// @param tree Registered commands.
/// @param preceding Tokens already accepted.
/// @param partial The partial token currently being typed.
/// @returns Completion candidates; may be empty.
auto SuggestCompletions(const CommandTree &tree,
                        const std::vector<std::string> &preceding,
                        const std::string &partial)
    -> std::vector<std::string>;

/// Schema-aware overload. When the user is typing the argument to
/// `set <path>` or `delete <path>`, candidates come from the schema
/// field tree rather than the command registry. Falls back to the
/// pure-tree behaviour for anything else.
/// @param tree Registered commands.
/// @param schema Loaded schema for the active adapter.
/// @param preceding Tokens already accepted.
/// @param partial The partial token currently being typed.
/// @returns Completion candidates; may be empty.
auto SuggestCompletions(const CommandTree &tree,
                        const schema::Schema &schema,
                        const std::vector<std::string> &preceding,
                        const std::string &partial)
    -> std::vector<std::string>;

/// Render the index of every command in the tree, one line per spec
/// sorted by path. Used by bare `help` / `?`.
/// @param tree Registered commands.
/// @returns Multi-line string ending with a trailing newline.
auto FormatHelpIndex(const CommandTree &tree) -> std::string;

/// Render help for a single command: path, role, argument list,
/// flags, and free-text description.
/// @param spec The spec to render.
/// @returns Formatted help string.
auto FormatCommandHelp(const CommandSpec &spec) -> std::string;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_COMMAND_TREE_H_
