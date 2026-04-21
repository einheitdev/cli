/// @file shell.h
/// @brief Interactive REPL. Owns the session lifecycle: detect TTY,
/// load history + aliases, read input via readline, dispatch via
/// CommandTree, render via Renderer, repeat until EOF or `exit`.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_SHELL_H_
#define INCLUDE_EINHEIT_CLI_SHELL_H_

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "einheit/cli/adapter.h"
#include "einheit/cli/auth.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/error.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/terminal_caps.h"
#include "einheit/cli/render/theme.h"
#include "einheit/cli/session.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::cli::shell {

/// Errors raised by the shell loop.
enum class ShellError {
  /// Transport failed to connect before the REPL could start.
  TransportUnavailable,
  /// Terminal setup / readline initialisation failed.
  TerminalInitFailed,
  /// Unrecoverable error inside the REPL loop.
  LoopFailed,
};

/// Everything the REPL needs at runtime. Constructed by the binary
/// main.cc; passed by reference into RunShell.
struct Shell {
  /// Connected transport. Owned here.
  std::unique_ptr<transport::Transport> tx;
  /// Loaded adapter for the running product. Owned here.
  std::unique_ptr<ProductAdapter> adapter;
  /// Registry of every command the user can run.
  CommandTree tree;
  /// Effective terminal capabilities.
  render::TerminalCaps caps;
  /// Colour palette used by every renderer in the shell. If left
  /// default-constructed, RunShell picks one via PickTheme(caps).
  std::optional<render::Theme> theme;
  /// Candidate-config session state.
  Session session;
  /// Resolved caller identity for this session.
  auth::CallerIdentity caller;
  /// True when running against the in-process learning daemon.
  /// Drives the startup banner and any learning-specific prompts.
  bool learning_mode = false;
  /// Resolved target name when `--target` / `einheit use` is active.
  std::string target_name;

  /// Optional path of a record file; every accepted command line
  /// is appended here as it's entered. Replay by feeding the file
  /// back via stdin or `--replay`.
  std::string record_path;

  /// Whether to print the status-chips line above every prompt.
  /// Off by default (some operators find the extra line busy);
  /// toggle with `--status-bar` or the in-shell `statusbar on`
  /// command.
  bool show_status_bar = false;

  /// Running totals for the session summary printed on exit.
  struct Stats {
    std::size_t commands = 0;
    std::size_t commits = 0;
    std::size_t rollbacks = 0;
    std::size_t errors = 0;
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
  } stats;

  /// Transport health snapshot, updated on every Dispatch that
  /// crosses the wire. Drives the status-bar connection chip.
  struct Health {
    /// True once at least one request has been acknowledged.
    bool has_response = false;
    /// Last observed wire round-trip time.
    std::chrono::milliseconds last_rtt{0};
    /// Last Dispatch result (ok / timeout / failed).
    enum class Status { Ok, Timeout, Failed } status = Status::Ok;
  } health;
};

/// Result of dispatching one accepted command line. Exposed so tests
/// can drive the dispatch path without spinning a real REPL.
struct DispatchResult {
  /// True when this line terminates the shell (`exit` / `quit`).
  bool exit_shell = false;
  /// True when the command was handled entirely in-process.
  bool handled_locally = false;
  /// Populated when the daemon returned a response.
  std::optional<protocol::Response> response;
};

/// Run a single parsed command against transport + session state.
/// Does not read input or render output; the REPL wraps this.
/// @param s Shell context (transport, adapter, tree, session).
/// @param parsed The parsed command to execute.
/// @returns DispatchResult, or a ShellError if the transport or
/// codec failed in an unrecoverable way.
auto Dispatch(Shell &s, const ParsedCommand &parsed)
    -> std::expected<DispatchResult, Error<ShellError>>;

/// Run the interactive REPL until the user exits. Blocking.
/// @param s Fully-populated Shell instance.
/// @returns void on clean exit, or ShellError.
auto RunShell(Shell &s) -> std::expected<void, Error<ShellError>>;

/// Execute a single command non-interactively: parse tokens, dispatch
/// once, render, return. Used when argv carries a command (e.g.
/// `einheit show tunnels` run from a script or CI). No REPL, no
/// history, no session — one-shot commands may not open configure.
/// @param s Fully-populated Shell instance (caller resolved, tx
/// connected, tree populated).
/// @param tokens Command-line tokens after argv flag parsing.
/// @returns DispatchResult on success, ShellError otherwise.
auto RunOneshot(Shell &s, const std::vector<std::string> &tokens)
    -> std::expected<DispatchResult, Error<ShellError>>;

}  // namespace einheit::cli::shell

#endif  // INCLUDE_EINHEIT_CLI_SHELL_H_
