/// @file globals.cc
/// @brief Framework-owned global command registration.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/globals.h"

#include <utility>

namespace einheit::cli {
namespace {

auto Make(std::string path, std::string wire, std::string help,
          RoleGate role = RoleGate::AnyAuthenticated,
          bool requires_session = false) -> CommandSpec {
  CommandSpec s;
  s.path = std::move(path);
  s.wire_command = std::move(wire);
  s.help = std::move(help);
  s.role = role;
  s.requires_session = requires_session;
  return s;
}

auto WithArg(CommandSpec s, std::string name, std::string help,
             bool required = true) -> CommandSpec {
  ArgSpec a;
  a.name = std::move(name);
  a.help = std::move(help);
  a.required = required;
  s.args.push_back(std::move(a));
  return s;
}

}  // namespace

auto RegisterGlobals(CommandTree &tree)
    -> std::expected<void, Error<CommandTreeError>> {
  const CommandSpec globals[] = {
      WithArg(Make("show config", "show_config",
                   "Show running configuration (optional prefix)"),
              "prefix", "Filter to paths under this prefix",
              /*required=*/false),
      Make("show commits", "show_commits", "List commit history"),
      WithArg(Make("show commit", "show_commit",
                   "Show a single commit by id"),
              "id", "Commit id from `show commits`"),
      // Framework-local: describes the adapter's schema without
      // contacting the daemon.
      Make("show schema", "",
           "List every configurable path with type + help"),
      Make("show env", "",
           "Show terminal caps, active theme, aliases, target, "
           "session"),
      Make("doctor", "",
           "Run framework health checks (transport, schema, theme, "
           "keys)"),
      Make("explain", "",
           "Show the wire representation + role + session "
           "requirement for a command"),
      Make("theme list", "", "List shipped named themes"),
      Make("theme use", "",
           "Switch to a named theme for the remainder of this "
           "session"),
      Make("statusbar", "",
           "Toggle the status chips line above the prompt: "
           "`statusbar on|off`"),
      Make("macro record", "",
           "Start recording a macro; `macro end` stops"),
      Make("macro end", "", "Stop recording the current macro"),
      Make("macro run", "", "Replay a saved macro by name"),
      Make("macro list", "", "List saved macros"),
      Make("macro show", "", "Show the commands in a saved macro"),
      Make("macro delete", "", "Remove a saved macro"),
      Make("configure", "configure",
           "Enter configure mode and open a candidate session",
           RoleGate::AdminOnly),
      Make("commit", "commit", "Apply the candidate configuration",
           RoleGate::AdminOnly, true),
      Make("rollback candidate", "rollback",
           "Discard the candidate session", RoleGate::AdminOnly,
           true),
      // Distinct wire verb so the daemon can tell `rollback
      // previous` apart from `rollback candidate` — the shell
      // strips path tokens that don't correspond to argument
      // slots, so otherwise both arrive as `rollback` with
      // empty args.
      Make("rollback previous", "rollback_previous",
           "Roll back to the previous commit", RoleGate::AdminOnly),
      Make("set", "set",
           "Set a candidate-config value at a schema path",
           RoleGate::AdminOnly, true),
      Make("delete", "delete",
           "Remove a candidate-config value at a schema path",
           RoleGate::AdminOnly, true),

      // Framework-local verbs (no wire round trip). The shell
      // intercepts these before dispatch; wire_command is left
      // empty as a marker.
      Make("help", "", "Show help for a command or topic"),
      Make("exit", "", "Exit the shell"),
      Make("quit", "", "Exit the shell"),
      Make("history", "", "Show the current user's command history"),
      Make("alias", "",
           "List aliases; `alias <name> <expansion...>` to define, "
           "`alias delete <name>` to remove. Persists to "
           "~/.einheit/aliases.yaml."),
      Make("watch", "", "Re-run a show command on event"),
      Make("logs", "", "Print daemon logs; use `logs --follow`"),
      Make("shell", "", "Drop to a POSIX shell (audit-logged)",
           RoleGate::AdminOnly),
  };

  for (const auto &g : globals) {
    if (auto r = Register(tree, g); !r) {
      return std::unexpected(r.error());
    }
  }
  return {};
}

}  // namespace einheit::cli
