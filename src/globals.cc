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

auto RegisterCoreGlobals(CommandTree &tree)
    -> std::expected<void, Error<CommandTreeError>> {
  // Always-on utility verbs. None depend on a candidate config, so a
  // product can never lose `help`/`exit` by opting out of config
  // (gap #6). Framework-local verbs carry an empty wire_command; the
  // shell intercepts them before dispatch.
  const CommandSpec core[] = {
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
      // Local-only counterparts to the daemon-owned
      // `daemon restart` / `daemon stop`. Start can't be
      // a wire verb (the daemon is stopped by definition
      // when you'd need it); status is cheap enough to
      // answer from `systemctl --user` without a wire
      // round-trip.
      Make("daemon start", "",
           "Start the daemon via systemd (local only)",
           RoleGate::AdminOnly),
      Make("daemon status", "",
           "Show the service's systemd status (local only)"),
  };

  for (const auto &g : core) {
    if (auto r = Register(tree, g); !r) {
      return std::unexpected(r.error());
    }
  }
  return {};
}

auto RegisterConfigGlobals(CommandTree &tree)
    -> std::expected<void, Error<CommandTreeError>> {
  // Candidate-config lifecycle + config-introspection verbs. Opt-in:
  // only meaningful when the product can actually hold and apply a
  // candidate.
  const CommandSpec config[] = {
      WithArg(Make("show config", "show_config",
                   "Show running configuration (optional prefix)"),
              "prefix", "Filter to paths under this prefix",
              /*required=*/false),
      Make("show diff", "show_diff",
           "Show uncommitted candidate changes vs running "
           "(Junos: show | compare)"),
      Make("show commits", "show_commits", "List commit history"),
      WithArg(Make("show commit", "show_commit",
                   "Show a single commit by id"),
              "id", "Commit id from `show commits`"),
      // Framework-local: describes the adapter's schema without
      // contacting the daemon.
      Make("show schema", "",
           "List every configurable path with type + help"),
      Make("configure", "configure",
           "Enter configure mode and open a candidate session",
           RoleGate::AdminOnly),
      Make("commit", "commit", "Apply the candidate configuration",
           RoleGate::AdminOnly, true),
      // commit-confirmed: apply AND arm a server-side auto-revert timer.
      // The anti-lockout feature — if `confirm` doesn't arrive within
      // the window, confd rolls back automatically.
      WithArg(Make("commit confirmed", "commit_confirmed",
                   "Apply, then auto-revert unless confirmed in time",
                   RoleGate::AdminOnly, true),
              "minutes", "Minutes to wait for `confirm`"),
      // Confirm an outstanding commit-confirmed window. No session
      // required — you reconnect after being locked out and confirm.
      Make("confirm", "confirm",
           "Confirm a pending commit-confirmed (cancel auto-revert)",
           RoleGate::AdminOnly),
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
      // Roll back to a specific committed revision by id. Distinct wire
      // verb so the id survives path-token stripping.
      WithArg(Make("rollback to", "rollback_to",
                   "Re-apply a specific committed revision",
                   RoleGate::AdminOnly),
              "id", "Commit id from `show commits`"),
      Make("set", "set",
           "Set a candidate-config value at a schema path",
           RoleGate::AdminOnly, true),
      Make("delete", "delete",
           "Remove a candidate-config value at a schema path",
           RoleGate::AdminOnly, true),
      // Service control wire verbs (`daemon restart`,
      // `daemon stop`) are no longer declared here — the
      // daemon advertises them through its `describe`
      // handshake, and the CLI picks them up at startup.
  };

  for (const auto &g : config) {
    if (auto r = Register(tree, g); !r) {
      return std::unexpected(r.error());
    }
  }
  return {};
}

auto RegisterGlobals(CommandTree &tree, const GlobalsOptions &opts)
    -> std::expected<void, Error<CommandTreeError>> {
  if (auto r = RegisterCoreGlobals(tree); !r) return r;
  if (opts.config_verbs) {
    if (auto r = RegisterConfigGlobals(tree); !r) return r;
  }
  return {};
}

auto RegisterGlobals(CommandTree &tree)
    -> std::expected<void, Error<CommandTreeError>> {
  return RegisterGlobals(tree, GlobalsOptions{});
}

}  // namespace einheit::cli
