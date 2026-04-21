/// @file shell.cc
/// @brief Interactive REPL + dispatch.
///
/// The REPL loop reads a line, parses it against the CommandTree,
/// and hands the result to Dispatch(). Dispatch handles framework-
/// local verbs in-process (exit/help/history/alias/watch) and
/// otherwise sends a wire Request, threading session state across
/// the configure → set → commit / rollback lifecycle.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/shell.h"

#include <chrono>
#include <exception>
#include <format>
#include <iostream>
#include <random>
#include <ranges>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef EINHEIT_HAVE_READLINE
#include <readline/history.h>
#endif

#include "einheit/cli/aliases.h"
#include "einheit/cli/audit.h"
#include "einheit/cli/auth.h"
#include "einheit/cli/history.h"
#include "einheit/cli/line_reader.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/banner.h"
#include "einheit/cli/render/confirm.h"
#include "einheit/cli/render/pager.h"
#include "einheit/cli/render/table.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::shell {
namespace {

auto Tokenize(const std::string &line) -> std::vector<std::string> {
  std::istringstream iss(line);
  std::vector<std::string> out;
  for (std::string t; iss >> t;) out.push_back(std::move(t));
  return out;
}

auto MakeError(ShellError code, std::string message)
    -> Error<ShellError> {
  return Error<ShellError>{code, std::move(message)};
}

auto NewRequestId() -> std::string {
  // Not a true UUID — MessagePack only carries it as a string and
  // the daemon just echoes it for correlation. A random hex blob
  // gets us uniqueness within a session.
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  return std::format("{:016x}{:016x}", rng(), rng());
}

auto StatusBar(const Shell &s) -> std::string {
  const bool color_ok =
      !s.caps.force_plain &&
      s.caps.colors != render::ColorDepth::None;
  constexpr const char *kReset = "\x1b[0m";
  constexpr const char *kDim = "\x1b[90m";
  constexpr const char *kGreen = "\x1b[32m";
  constexpr const char *kYellow = "\x1b[33m";
  constexpr const char *kRed = "\x1b[31m";
  const auto c = [&](const char *ansi, const std::string &t) {
    return color_ok ? std::format("{}{}{}", ansi, t, kReset) : t;
  };

  std::vector<std::string> chips;
  chips.push_back(
      c(kDim, std::format("● {} cmds", s.stats.commands)));
  if (s.stats.commits > 0) {
    chips.push_back(
        c(kGreen, std::format("✓ {} commits", s.stats.commits)));
  }
  if (s.stats.errors > 0) {
    chips.push_back(
        c(kRed, std::format("✗ {} errors", s.stats.errors)));
  }
  if (s.session.in_configure) {
    chips.push_back(c(kYellow, "◆ configure"));
  }
  if (s.session.confirm_deadline) {
    chips.push_back(c(kYellow, "⏱ commit-confirm pending"));
  }

  std::string out;
  for (std::size_t i = 0; i < chips.size(); ++i) {
    if (i > 0) out += c(kDim, " · ");
    out += chips[i];
  }
  return out.empty() ? std::string{} : out + "\n";
}

auto PromptFor(const Shell &s, const ProductMetadata &meta)
    -> std::string {
  const bool color_ok =
      !s.caps.force_plain &&
      s.caps.colors != render::ColorDepth::None;
  const std::string host = s.target_name.empty()
                               ? meta.prompt
                               : s.target_name;
  const std::string prefix =
      s.caller.user.empty() ? host
                             : std::format("{}@{}", s.caller.user,
                                           host);
  const char glyph = s.session.in_configure ? '#' : '>';
  if (!color_ok) {
    return std::format("{}{} ", prefix, glyph);
  }
  // dim the user@host, colour the mode glyph (yellow in configure,
  // cyan in operational) so mode is always visually obvious.
  constexpr const char *kReset = "\x1b[0m";
  constexpr const char *kDim = "\x1b[90m";
  const char *mode_color =
      s.session.in_configure ? "\x1b[33m" : "\x1b[36m";
  return std::format("{}{}{} {}{}{} ", kDim, prefix, kReset,
                     mode_color, glyph, kReset);
}

auto BuildRequest(const ParsedCommand &parsed, const Shell &s)
    -> protocol::Request {
  protocol::Request req;
  req.id = NewRequestId();
  req.command = parsed.spec->wire_command;
  req.args = parsed.args;
  audit::StampIdentity(s.caller, req.user, req.role);
  if (parsed.spec->requires_session && s.session.session_id) {
    req.session_id = *s.session.session_id;
  }
  return req;
}

// Extract the daemon-issued session id out of a commit/configure
// Response. The fake daemon encodes it in the `data` blob — for now
// we accept any non-empty string there as the id. Real adapters
// decode via their own response shapes.
auto ExtractSessionIdFromData(
    const std::vector<std::uint8_t> &bytes) -> std::optional<std::string> {
  if (bytes.empty()) return std::nullopt;
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

auto Dispatch(Shell &s, const ParsedCommand &parsed)
    -> std::expected<DispatchResult, Error<ShellError>> {
  DispatchResult out;
  const auto &spec = *parsed.spec;

  // Framework-local verbs never cross the wire.
  if (spec.wire_command.empty()) {
    out.handled_locally = true;
    if (spec.path == "exit" || spec.path == "quit") {
      // Junos-style two-level exit: inside configure we drop the
      // candidate session and return to operational; the second
      // exit from operational closes the shell.
      if (s.session.in_configure) {
        // Ask the daemon to drop the candidate server-side too.
        ParsedCommand rb;
        CommandSpec rb_spec;
        rb_spec.path = "rollback candidate";
        rb_spec.wire_command = "rollback";
        rb_spec.requires_session = true;
        rb_spec.role = RoleGate::AdminOnly;
        rb.spec = &rb_spec;
        rb.args = {"candidate"};
        auto req = BuildRequest(rb, s);
        using namespace std::chrono_literals;
        (void)s.tx->SendRequest(req, 2s);
        ClearSession(s.session);
      } else {
        out.exit_shell = true;
      }
    }
    return out;
  }

  // Session invariants before sending.
  if (spec.requires_session && !s.session.in_configure) {
    return std::unexpected(MakeError(
        ShellError::LoopFailed,
        std::format("command requires 'configure' session: {}",
                    spec.path)));
  }

  auto req = BuildRequest(parsed, s);
  using namespace std::chrono_literals;
  auto resp = s.tx->SendRequest(req, 30s);
  if (!resp) {
    return std::unexpected(
        MakeError(ShellError::LoopFailed, resp.error().message));
  }

  // Update session state on lifecycle verbs.
  if (resp->status == protocol::ResponseStatus::Ok) {
    if (spec.wire_command == "configure") {
      s.session.in_configure = true;
      s.session.session_id = ExtractSessionIdFromData(resp->data);
    } else if (spec.wire_command == "commit" ||
               spec.path == "rollback candidate") {
      ClearSession(s.session);
    } else if (spec.wire_command == "rollback") {
      // rollback previous / rollback to N don't touch the session.
    }
  }

  out.response = std::move(*resp);
  return out;
}

auto RunShell(Shell &s) -> std::expected<void, Error<ShellError>> {
  if (!s.tx || !s.adapter) {
    return std::unexpected(MakeError(
        ShellError::TransportUnavailable, "shell not initialised"));
  }
  if (s.caller.user.empty()) {
    auto caller = auth::ResolveLocal();
    if (!caller) {
      return std::unexpected(
          MakeError(ShellError::LoopFailed, caller.error().message));
    }
    s.caller = *caller;
  }

  const auto meta = s.adapter->Metadata();

  render::BannerInfo binfo;
  binfo.product_name = meta.display_name.empty() ? meta.id
                                                 : meta.display_name;
  binfo.adapter_name = meta.id;
  binfo.version = meta.version;
  binfo.learning_mode = s.learning_mode;
  binfo.target_name = s.target_name;
  binfo.tip = render::PickTip();
  std::cout << render::Banner(binfo, s.caps);

  // Mini tutorial in learning mode — gives first-time users a
  // concrete path to try.
  if (s.learning_mode) {
    const bool colorful =
        !s.caps.force_plain &&
        s.caps.colors != render::ColorDepth::None;
    constexpr const char *kDim = "\x1b[90m";
    constexpr const char *kCyan = "\x1b[36m";
    constexpr const char *kReset = "\x1b[0m";
    const auto c = [&](const char *ansi, const std::string &t) {
      return colorful ? std::format("{}{}{}", ansi, t, kReset) : t;
    };
    std::cout << c(kDim, "try:  ") << c(kCyan, "show schema")
              << c(kDim, "  →  ") << c(kCyan, "configure")
              << c(kDim, "  →  ")
              << c(kCyan, "set hostname demo")
              << c(kDim, "  →  ") << c(kCyan, "commit")
              << "\n\n";
  }

  s.stats.start = std::chrono::steady_clock::now();

  render::Renderer renderer(std::cout, s.caps);

  // Best-effort history load; absence of a backing file is fine.
  History history;
  if (auto h = Load(s.caller.user); h) history = *h;

  // Merge legacy k=v aliases with the YAML file at
  // ~/.einheit/aliases.yaml (if present). YAML wins on conflict.
  Aliases aliases;
  if (auto a = LoadAliases(s.caller.user); a) aliases = *a;
  if (const auto yaml_path = DefaultYamlPath(); !yaml_path.empty()) {
    if (auto y = LoadAliasesYaml(yaml_path); y) {
      MergeAliases(aliases, *y);
    }
  }

  auto reader = NewLineReader();
  reader->SetCompletion(
      [&](const std::vector<std::string> &preceding,
          const std::string &partial) -> std::vector<std::string> {
        return SuggestCompletions(s.tree, s.adapter->GetSchema(),
                                  preceding, partial);
      });
  reader->SetHelp(
      [&](const std::vector<std::string> &preceding,
          const std::string &partial)
          -> std::vector<HelpCandidate> {
        auto names = SuggestCompletions(
            s.tree, s.adapter->GetSchema(), preceding, partial);
        std::vector<HelpCandidate> out;
        out.reserve(names.size());
        for (const auto &name : names) {
          HelpCandidate c;
          c.name = name;
          std::string full_path;
          for (const auto &p : preceding) {
            if (!full_path.empty()) full_path += ' ';
            full_path += p;
          }
          if (!full_path.empty()) full_path += ' ';
          full_path += name;
          auto it = s.tree.by_path.find(full_path);
          if (it != s.tree.by_path.end()) {
            c.help = it->second.help;
          } else {
            for (const auto &[path, spec] : s.tree.by_path) {
              if (path.rfind(full_path + " ", 0) == 0) {
                c.help =
                    std::format("(more under `{}`)", full_path);
                break;
              }
            }
          }
          out.push_back(std::move(c));
        }
        return out;
      });

  while (true) {
    const auto status = StatusBar(s);
    auto raw = reader->ReadLine(
        std::format("{}{}", status, PromptFor(s, meta)));
    if (!raw) break;

    // History expansion: `!!` reruns the last entry, `!N` reruns
    // entry N, `!pfx` reruns the last matching prefix. Failure
    // (e.g. no prior entry) falls back to the literal input.
    std::string expanded = *raw;
#ifdef EINHEIT_HAVE_READLINE
    {
      char *result = nullptr;
      const int rc =
          ::history_expand(expanded.data(), &result);
      if (rc >= 0 && result) {
        expanded = result;
        if (rc == 1) {
          // Show the expanded form — matches bash behaviour.
          std::cout << expanded << '\n';
        }
      }
      if (result) std::free(result);
    }
#endif

    const auto line = Expand(aliases, expanded);
    if (line.empty()) continue;
    reader->AddHistory(line);

    auto tokens = Tokenize(line);
    auto parsed = Parse(s.tree, tokens, s.caller.role);
    if (!parsed) {
      s.stats.errors += 1;
      // Split "message — did you mean 'X'?" into message + hint so
      // the suggestion lands on the yellow hint line of the box.
      std::string msg = parsed.error().message;
      std::string hint;
      if (const auto pos = msg.find(" — ");
          pos != std::string::npos) {
        hint = msg.substr(pos + 5);  // skip " — "
        msg = msg.substr(0, pos);
      }
      render::RenderError("parse", msg, hint, renderer);
      continue;
    }

    // Destructive verbs get a yellow confirmation prompt.
    if (parsed->spec->path == "rollback previous" ||
        parsed->spec->path == "shell" ||
        parsed->spec->path == "delete") {
      const auto msg =
          std::format("about to run `{}` — this cannot be undone.",
                      parsed->spec->path);
      if (!render::ConfirmPrompt(msg, std::cout, std::cin,
                                 s.caps)) {
        std::cout << "  cancelled.\n";
        continue;
      }
    }

    auto result = Dispatch(s, *parsed);
    if (result) {
      (void)Append(history, line);
      s.stats.commands += 1;
      if (parsed->spec->wire_command == "commit" &&
          result->response &&
          result->response->status == protocol::ResponseStatus::Ok) {
        s.stats.commits += 1;
      } else if (parsed->spec->wire_command == "rollback" &&
                 result->response &&
                 result->response->status ==
                     protocol::ResponseStatus::Ok) {
        s.stats.rollbacks += 1;
      }
      if (result->response &&
          result->response->status ==
              protocol::ResponseStatus::Error) {
        s.stats.errors += 1;
      }
    }
    if (!result) {
      s.stats.errors += 1;
      render::RenderError("dispatch", result.error().message, "",
                          renderer);
      continue;
    }
    if (result->exit_shell) break;
    if (result->handled_locally) {
      if (parsed->spec->path == "help") {
        if (parsed->args.empty()) {
          std::cout << FormatHelpIndex(s.tree);
        } else {
          std::string target_path;
          for (std::size_t i = 0; i < parsed->args.size(); ++i) {
            if (i > 0) target_path += ' ';
            target_path += parsed->args[i];
          }
          auto it = s.tree.by_path.find(target_path);
          if (it == s.tree.by_path.end()) {
            std::cerr << std::format(
                "help: no such command '{}'\n", target_path);
          } else {
            std::cout << FormatCommandHelp(it->second);
          }
        }
      } else if (parsed->spec->path == "show schema") {
        const std::string prefix =
            parsed->args.empty() ? "" : parsed->args[0];
        std::cout << schema::FormatSchema(s.adapter->GetSchema(),
                                          prefix);
      }
      continue;
    }
    if (result->response) {
      // Buffer the adapter's output so we can decide whether to
      // page it based on line count vs terminal height.
      std::ostringstream buf;
      render::Renderer buffered(buf, s.caps, renderer.Format());
      try {
        s.adapter->RenderResponse(*parsed->spec, *result->response,
                                  buffered);
      } catch (const std::exception &e) {
        std::cerr << "render: " << e.what() << '\n';
      }
      render::Flush(buf.str(), s.caps);
    }
  }

  // Session-end summary.
  {
    const auto elapsed =
        std::chrono::steady_clock::now() - s.stats.start;
    const auto secs =
        std::chrono::duration_cast<std::chrono::seconds>(elapsed)
            .count();
    render::Table t;
    AddColumn(t, "session", render::Align::Left,
              render::Priority::High);
    AddColumn(t, "count", render::Align::Right,
              render::Priority::High);
    AddRow(t, {render::Cell{"commands",
                            render::Semantic::Emphasis},
               render::Cell{std::to_string(s.stats.commands)}});
    AddRow(t, {render::Cell{"commits",
                            render::Semantic::Emphasis},
               render::Cell{std::to_string(s.stats.commits),
                            render::Semantic::Good}});
    AddRow(t, {render::Cell{"rollbacks",
                            render::Semantic::Emphasis},
               render::Cell{std::to_string(s.stats.rollbacks)}});
    AddRow(t, {render::Cell{"errors",
                            render::Semantic::Emphasis},
               render::Cell{std::to_string(s.stats.errors),
                            s.stats.errors > 0
                                ? render::Semantic::Bad
                                : render::Semantic::Dim}});
    AddRow(t, {render::Cell{"duration",
                            render::Semantic::Emphasis},
               render::Cell{std::format("{}s", secs),
                            render::Semantic::Dim}});
    Render(t, std::cout, s.caps);
  }
  return {};
}

auto RunOneshot(Shell &s, const std::vector<std::string> &tokens)
    -> std::expected<DispatchResult, Error<ShellError>> {
  if (!s.tx || !s.adapter) {
    return std::unexpected(MakeError(
        ShellError::TransportUnavailable, "shell not initialised"));
  }
  if (s.caller.user.empty()) {
    auto caller = auth::ResolveLocal();
    if (!caller) {
      return std::unexpected(
          MakeError(ShellError::LoopFailed, caller.error().message));
    }
    s.caller = *caller;
  }

  auto parsed = Parse(s.tree, tokens, s.caller.role);
  if (!parsed) {
    return std::unexpected(
        MakeError(ShellError::LoopFailed, parsed.error().message));
  }
  if (parsed->spec->requires_session) {
    return std::unexpected(MakeError(
        ShellError::LoopFailed,
        std::format("oneshot cannot open configure session: {}",
                    parsed->spec->path)));
  }

  auto result = Dispatch(s, *parsed);
  if (!result) return std::unexpected(result.error());

  if (result->response) {
    render::Renderer renderer(std::cout, s.caps);
    try {
      s.adapter->RenderResponse(*parsed->spec, *result->response,
                                renderer);
    } catch (const std::exception &e) {
      return std::unexpected(
          MakeError(ShellError::LoopFailed, e.what()));
    }
  }
  return result;
}

}  // namespace einheit::cli::shell
