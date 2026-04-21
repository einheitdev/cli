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
#include <filesystem>
#include <format>
#include <fstream>
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
#include "einheit/cli/workstation_state.h"

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

auto StatusBar(const Shell &s, const render::Theme &theme)
    -> std::string {
  const bool color_ok =
      !s.caps.force_plain &&
      s.caps.colors != render::ColorDepth::None;
  constexpr const char *kReset = "\x1b[0m";
  const auto dim = color_ok ? render::FgAnsi(theme.dim)
                             : std::string();
  const auto good = color_ok ? render::FgAnsi(theme.good)
                              : std::string();
  const auto warn = color_ok ? render::FgAnsi(theme.warn)
                              : std::string();
  const auto bad = color_ok ? render::FgAnsi(theme.bad)
                             : std::string();
  const auto c = [&](const std::string &ansi,
                     const std::string &t) {
    return color_ok ? std::format("{}{}{}", ansi, t, kReset) : t;
  };

  std::vector<std::string> chips;
  // Health chip: green ● when the last round-trip was fast, yellow
  // when slow, red on timeout/failure, dim grey until the first
  // exchange lands.
  if (!s.health.has_response) {
    chips.push_back(c(dim, "◌ waiting"));
  } else if (s.health.status == Shell::Health::Status::Timeout) {
    chips.push_back(c(bad, "⏱ timed out"));
  } else if (s.health.status == Shell::Health::Status::Failed) {
    chips.push_back(c(bad, "✗ transport"));
  } else {
    const auto ms = s.health.last_rtt.count();
    const auto &chip_color =
        ms > 200 ? warn : (ms > 50 ? dim : good);
    chips.push_back(
        c(chip_color, std::format("◉ {}ms", ms)));
  }
  chips.push_back(
      c(dim, std::format("● {} cmds", s.stats.commands)));
  if (s.stats.commits > 0) {
    chips.push_back(
        c(good, std::format("✓ {} commits", s.stats.commits)));
  }
  if (s.stats.errors > 0) {
    chips.push_back(
        c(bad, std::format("✗ {} errors", s.stats.errors)));
  }
  if (s.session.in_configure) {
    chips.push_back(c(warn, "◆ configure"));
  }
  if (s.session.confirm_deadline) {
    chips.push_back(c(warn, "⏱ commit-confirm pending"));
  }

  std::string out;
  for (std::size_t i = 0; i < chips.size(); ++i) {
    if (i > 0) out += c(dim, " · ");
    out += chips[i];
  }
  return out.empty() ? std::string{} : out + "\n";
}

auto PromptFor(const Shell &s, const ProductMetadata &meta,
               const render::Theme &theme) -> std::string {
  const bool color_ok =
      !s.caps.force_plain &&
      s.caps.colors != render::ColorDepth::None;
  const std::string host = s.target_name.empty()
                               ? meta.prompt
                               : s.target_name;
  const char glyph = s.session.in_configure ? '#' : '>';

  if (!color_ok) {
    const std::string prefix =
        s.caller.user.empty()
            ? host
            : std::format("{}@{}", s.caller.user, host);
    return std::format("{}{} ", prefix, glyph);
  }

  // Wrap every ANSI escape in readline's "non-printing" brackets
  // (\x01 .. \x02) so readline doesn't count them toward the
  // cursor column. Without these, typing past the assumed prompt
  // width corrupts the line as characters reach the right edge.
  constexpr const char *kBegin = "\x01";
  constexpr const char *kEnd = "\x02";
  const auto wrap = [&](const std::string &ansi) {
    return std::format("{}{}{}", kBegin, ansi, kEnd);
  };
  const auto reset = wrap("\x1b[0m");
  const auto user_color = wrap(render::FgAnsi(theme.prompt_user));
  const auto at_color = wrap(render::FgAnsi(theme.prompt_at));
  const auto host_color = wrap(render::FgAnsi(theme.prompt_host));
  const auto mode_color = wrap(render::FgAnsi(
      s.session.in_configure ? theme.warn : theme.accent));

  std::string prefix;
  if (s.caller.user.empty()) {
    prefix = std::format("{}{}{}", host_color, host, reset);
  } else {
    prefix = std::format("{}{}{}{}@{}{}{}{}", user_color,
                         s.caller.user, reset, at_color, reset,
                         host_color, host, reset);
  }
  return std::format("{} {}{}{} ", prefix, mode_color, glyph,
                     reset);
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
  const auto t0 = std::chrono::steady_clock::now();
  auto resp = s.tx->SendRequest(req, 30s);
  s.health.last_rtt =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0);
  if (!resp) {
    s.health.status =
        resp.error().code == transport::TransportError::Timeout
            ? Shell::Health::Status::Timeout
            : Shell::Health::Status::Failed;
    return std::unexpected(
        MakeError(ShellError::LoopFailed, resp.error().message));
  }
  s.health.has_response = true;
  s.health.status = Shell::Health::Status::Ok;

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
  const auto theme = s.theme.value_or(
      render::PickTheme(s.caps, render::DetectLightTerminal()));
  std::cout << render::Banner(binfo, s.caps, theme);

  // Mini tutorial in learning mode — gives first-time users a
  // concrete path to try. Uses theme colours so it matches the
  // rest of the shell chrome.
  if (s.learning_mode) {
    const bool colorful =
        !s.caps.force_plain &&
        s.caps.colors != render::ColorDepth::None;
    constexpr const char *kReset = "\x1b[0m";
    const auto dim = colorful ? render::FgAnsi(theme.dim)
                               : std::string();
    const auto accent = colorful ? render::FgAnsi(theme.accent)
                                  : std::string();
    const auto c = [&](const std::string &ansi,
                       const std::string &t) {
      return colorful ? std::format("{}{}{}", ansi, t, kReset) : t;
    };
    std::cout << c(dim, "try:  ") << c(accent, "show schema")
              << c(dim, "  →  ") << c(accent, "configure")
              << c(dim, "  →  ")
              << c(accent, "set hostname demo")
              << c(dim, "  →  ") << c(accent, "commit")
              << "\n\n";
  }

  s.stats.start = std::chrono::steady_clock::now();

  render::Renderer renderer(std::cout, s.caps,
                            render::OutputFormat::Table, theme);

  // Best-effort history load; absence of a backing file is fine.
  History history;
  if (auto h = Load(s.caller.user); h) history = *h;

  // Optional recording file. Every accepted command lands here,
  // one per line, so `einheit --replay file` (or piping the file
  // back as stdin) re-runs the same session.
  std::unique_ptr<std::ofstream> record;
  if (!s.record_path.empty()) {
    std::filesystem::create_directories(
        std::filesystem::path(s.record_path).parent_path());
    record = std::make_unique<std::ofstream>(s.record_path,
                                             std::ios::trunc);
    if (!record->is_open()) {
      std::cerr << std::format(
          "record: could not open {}\n", s.record_path);
      record.reset();
    } else {
      *record << std::format(
          "# einheit session recording — {} commands follow\n",
          s.caller.user);
    }
  }

  // Merge legacy k=v aliases with the YAML file at
  // ~/.einheit/aliases.yaml (if present). YAML wins on conflict.
  Aliases aliases;
  if (auto a = LoadAliases(s.caller.user); a) aliases = *a;
  if (const auto yaml_path = DefaultYamlPath(); !yaml_path.empty()) {
    if (std::filesystem::exists(yaml_path)) {
      if (auto y = LoadAliasesYaml(yaml_path); y) {
        MergeAliases(aliases, *y);
      } else {
        // Surface YAML parse errors — silent failure here bit us
        // in a previous bug where the file was in the wrong HOME.
        std::cerr << std::format("alias: {} ({})\n",
                                 y.error().message, yaml_path);
      }
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
    // Status bar is its own line above the prompt so readline
    // doesn't see an embedded newline or unwrapped ANSI in what
    // it thinks is a single-line prompt.
    if (const auto status = StatusBar(s, theme); !status.empty()) {
      std::cout << status << std::flush;
    }
    auto raw = reader->ReadLine(PromptFor(s, meta, theme));
    if (!raw) break;
    // Comment lines let recorded session files carry annotations.
    if (!raw->empty() && (*raw)[0] == '#') continue;

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
    if (record) {
      *record << line << '\n';
      record->flush();
    }

    auto tokens = Tokenize(line);
    // `time <cmd>` prefix — strip it, wrap the dispatch with a
    // wall-clock timer and print the total at the end.
    bool time_it = false;
    if (tokens.size() >= 2 && tokens.front() == "time") {
      time_it = true;
      tokens.erase(tokens.begin());
    }
    const auto cmd_start = std::chrono::steady_clock::now();

    auto parsed = Parse(s.tree, tokens, s.caller.role);
    if (!parsed) {
      s.stats.errors += 1;
      std::string msg = parsed.error().message;
      std::string hint;
      if (const auto pos = msg.find(" — ");
          pos != std::string::npos) {
        hint = msg.substr(pos + 5);
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
    // Retry-on-timeout: if the transport timed out, offer one
    // interactive retry. Only prompts for prefix `Timeout` hints —
    // other transport failures (auth, codec) are not retryable.
    if (!result &&
        s.health.status == Shell::Health::Status::Timeout) {
      if (render::ConfirmPrompt(
              "request timed out — retry with the same command?",
              std::cout, std::cin, s.caps)) {
        result = Dispatch(s, *parsed);
      }
    }
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
          auto tbl = BuildHelpIndex(s.tree);
          render::RenderFormatted(tbl, renderer);
        } else {
          std::string target_path;
          for (std::size_t i = 0; i < parsed->args.size(); ++i) {
            if (i > 0) target_path += ' ';
            target_path += parsed->args[i];
          }
          auto it = s.tree.by_path.find(target_path);
          if (it == s.tree.by_path.end()) {
            render::RenderError(
                "help",
                std::format("no such command '{}'", target_path),
                "", renderer);
          } else {
            auto tbl = BuildCommandHelp(it->second);
            render::RenderFormatted(tbl, renderer);
          }
        }
      } else if (parsed->spec->path == "show schema") {
        const std::string prefix =
            parsed->args.empty() ? "" : parsed->args[0];
        auto tbl =
            schema::BuildSchema(s.adapter->GetSchema(), prefix);
        render::RenderFormatted(tbl, renderer);
      } else if (parsed->spec->path == "explain") {
        if (parsed->args.empty()) {
          render::RenderError("explain",
                              "usage: explain <command-path>", "",
                              renderer);
        } else {
          std::string target;
          for (std::size_t i = 0; i < parsed->args.size(); ++i) {
            if (i > 0) target += ' ';
            target += parsed->args[i];
          }
          auto it = s.tree.by_path.find(target);
          if (it == s.tree.by_path.end()) {
            render::RenderError(
                "explain",
                std::format("no such command '{}'", target), "",
                renderer);
          } else {
            const auto &sp = it->second;
            render::Table t;
            render::AddColumn(t, "field", render::Align::Left,
                              render::Priority::High);
            render::AddColumn(t, "value", render::Align::Left,
                              render::Priority::High);
            const auto add = [&](const std::string &k,
                                 const std::string &v,
                                 render::Semantic sem =
                                     render::Semantic::Default) {
              render::AddRow(t, {
                  render::Cell{k, render::Semantic::Emphasis},
                  render::Cell{v, sem},
              });
            };
            add("path", sp.path, render::Semantic::Info);
            add("wire_command",
                sp.wire_command.empty() ? "<framework-local>"
                                         : sp.wire_command,
                sp.wire_command.empty() ? render::Semantic::Dim
                                         : render::Semantic::Good);
            add("role",
                sp.role == RoleGate::AdminOnly         ? "admin"
                : sp.role == RoleGate::OperatorOrAdmin ? "operator"
                                                       : "any",
                sp.role == RoleGate::AdminOnly
                    ? render::Semantic::Warn
                    : render::Semantic::Dim);
            add("requires_session",
                sp.requires_session ? "yes" : "no",
                sp.requires_session ? render::Semantic::Warn
                                     : render::Semantic::Dim);
            add("args", std::format("{}", sp.args.size()));
            // Sketch of the wire envelope.
            std::string envelope = std::format(
                R"({{"command":"{}","args":[...],"session_id":{}}})",
                sp.wire_command.empty() ? "<local>"
                                         : sp.wire_command,
                sp.requires_session ? "\"<sid>\"" : "null");
            add("envelope", envelope, render::Semantic::Info);
            render::RenderFormatted(t, renderer);
          }
        }
      } else if (parsed->spec->path == "show env") {
        render::Table t;
        render::AddColumn(t, "field", render::Align::Left,
                          render::Priority::High);
        render::AddColumn(t, "value", render::Align::Left,
                          render::Priority::High);
        const auto add = [&](const std::string &k,
                             const std::string &v,
                             render::Semantic sem =
                                 render::Semantic::Default) {
          render::AddRow(t, {
              render::Cell{k, render::Semantic::Emphasis},
              render::Cell{v, sem},
          });
        };
        add("colors",
            s.caps.colors == render::ColorDepth::TrueColor
                ? "truecolor"
                : s.caps.colors == render::ColorDepth::Ansi256
                      ? "256"
                      : s.caps.colors == render::ColorDepth::Ansi16
                            ? "16"
                            : "none",
            render::Semantic::Info);
        add("unicode", s.caps.unicode ? "yes" : "no",
            s.caps.unicode ? render::Semantic::Good
                            : render::Semantic::Dim);
        add("width", std::format("{}", s.caps.width));
        add("height", std::format("{}", s.caps.height));
        add("is_tty", s.caps.is_tty ? "yes" : "no");
        add("target",
            s.target_name.empty() ? "<local>" : s.target_name,
            render::Semantic::Info);
        add("user", s.caller.user);
        add("configure",
            s.session.in_configure ? "yes" : "no",
            s.session.in_configure ? render::Semantic::Warn
                                    : render::Semantic::Dim);
        add("learning_mode", s.learning_mode ? "yes" : "no",
            s.learning_mode ? render::Semantic::Warn
                             : render::Semantic::Dim);
        add("aliases",
            std::format("{} loaded", aliases.table.size()));
        add("history",
            std::format("{} entries", history.entries.size()));
        render::RenderFormatted(t, renderer);
      } else if (parsed->spec->path == "doctor") {
        render::Table t;
        render::AddColumn(t, "check", render::Align::Left,
                          render::Priority::High);
        render::AddColumn(t, "result", render::Align::Left,
                          render::Priority::High);
        render::AddColumn(t, "detail", render::Align::Left,
                          render::Priority::Medium);
        const auto row = [&](const std::string &check, bool ok,
                             const std::string &detail) {
          render::AddRow(t, {
              render::Cell{check, render::Semantic::Emphasis},
              render::Cell{ok ? "✓ pass" : "✗ fail",
                           ok ? render::Semantic::Good
                              : render::Semantic::Bad},
              render::Cell{detail, render::Semantic::Dim},
          });
        };
        row("transport attached", s.tx != nullptr,
            s.tx ? "ok" : "no transport");
        row("adapter attached", s.adapter != nullptr,
            s.adapter ? s.adapter->Metadata().id : "");
        row("schema loaded",
            !s.adapter->GetSchema().product.empty(),
            s.adapter->GetSchema().product);
        row("last round-trip",
            s.health.has_response &&
                s.health.status ==
                    Shell::Health::Status::Ok,
            s.health.has_response
                ? std::format("{}ms", s.health.last_rtt.count())
                : "no round-trip yet");
        row("theme loaded", s.theme.has_value(),
            s.theme ? "custom" : "default dark");
        row("aliases loaded", !aliases.table.empty(),
            std::format("{} entries", aliases.table.size()));
        render::RenderFormatted(t, renderer);
      } else if (parsed->spec->path == "theme list") {
        render::Table t;
        render::AddColumn(t, "theme", render::Align::Left,
                          render::Priority::High);
        render::AddColumn(t, "description", render::Align::Left,
                          render::Priority::Medium);
        const auto descs =
            std::unordered_map<std::string, std::string>{
                {"forest", "earthy greens + moss"},
                {"high-contrast", "near-mono, max legibility"},
                {"ocean", "cool blues + teal"},
                {"psychotropic", "default, bold and playful"},
                {"solarized-dark", "Ethan Schoonover's palette"},
            };
        for (const auto &name : render::NamedThemeList()) {
          render::AddRow(t, {
              render::Cell{name, render::Semantic::Emphasis},
              render::Cell{descs.count(name) ? descs.at(name) : "",
                           render::Semantic::Dim},
          });
        }
        render::RenderFormatted(t, renderer);
      } else if (parsed->spec->path == "theme use") {
        if (parsed->args.empty()) {
          render::RenderError("theme",
                              "usage: theme use <name>",
                              "try: theme list", renderer);
        } else {
          const auto &name = parsed->args[0];
          auto picked = render::NamedTheme(name);
          if (!picked) {
            render::RenderError(
                "theme", std::format("unknown theme '{}'", name),
                "try: theme list", renderer);
          } else {
            // Swap the live theme and persist the choice so the
            // next invocation picks it up too.
            s.theme = *picked;
            renderer = render::Renderer(
                std::cout, s.caps, renderer.Format(), *picked);
            workstation::State st;
            if (auto prior = workstation::Load(
                    workstation::DefaultPath());
                prior) {
              st = *prior;
            }
            st.active_theme = name;
            (void)workstation::Save(workstation::DefaultPath(), st);
            std::cout << std::format("  theme → `{}`\n", name);
          }
        }
      } else if (parsed->spec->path == "alias") {
        // `alias`                       → list
        // `alias <name> <expansion...>` → define or replace
        // `alias delete <name>`         → remove
        const auto yaml_path = DefaultYamlPath();
        if (parsed->args.empty()) {
          render::Table t;
          render::AddColumn(t, "alias", render::Align::Left,
                            render::Priority::High);
          render::AddColumn(t, "expands to", render::Align::Left,
                            render::Priority::High);
          render::AddColumn(t, "help", render::Align::Left,
                            render::Priority::Medium);
          std::vector<std::string> keys;
          keys.reserve(aliases.table.size());
          for (const auto &[k, _] : aliases.table) keys.push_back(k);
          std::sort(keys.begin(), keys.end());
          for (const auto &k : keys) {
            const auto help_it = aliases.help.find(k);
            render::AddRow(t, {
                render::Cell{k, render::Semantic::Emphasis},
                render::Cell{aliases.table.at(k),
                             render::Semantic::Info},
                render::Cell{help_it != aliases.help.end()
                                 ? help_it->second
                                 : std::string{},
                             render::Semantic::Dim},
            });
          }
          if (keys.empty()) {
            std::cout << "  (no aliases — try: "
                         "alias st show status)\n";
          } else {
            render::RenderFormatted(t, renderer);
          }
        } else if (parsed->args[0] == "delete" ||
                   parsed->args[0] == "remove" ||
                   parsed->args[0] == "rm") {
          if (parsed->args.size() < 2) {
            render::RenderError(
                "alias", "usage: alias delete <name>", "",
                renderer);
          } else {
            const auto &name = parsed->args[1];
            if (auto r = RemoveYamlAlias(yaml_path, name); !r) {
              render::RenderError("alias", r.error().message, "",
                                  renderer);
            } else {
              aliases.table.erase(name);
              aliases.help.erase(name);
              std::cout << std::format("  removed alias `{}`\n",
                                       name);
            }
          }
        } else if (parsed->args.size() < 2) {
          render::RenderError(
              "alias",
              "usage: alias [<name> <expansion...>] | "
              "delete <name>",
              "", renderer);
        } else {
          // Define: first arg is the name, rest joined as
          // expansion.
          const auto name = parsed->args[0];
          std::string expansion;
          for (std::size_t i = 1; i < parsed->args.size(); ++i) {
            if (!expansion.empty()) expansion += ' ';
            expansion += parsed->args[i];
          }
          if (auto r = SetYamlAlias(yaml_path, name, expansion);
              !r) {
            render::RenderError("alias", r.error().message, "",
                                renderer);
          } else {
            aliases.table[name] = expansion;
            std::cout << std::format(
                "  alias `{}` → `{}`\n  saved to {}\n", name,
                expansion, yaml_path);
          }
        }
      }
      continue;
    }
    if (result->response) {
      // Buffer the adapter's output so we can decide whether to
      // page it based on line count vs terminal height.
      std::ostringstream buf;
      render::Renderer buffered(buf, s.caps, renderer.Format(),
                                renderer.GetTheme());
      try {
        s.adapter->RenderResponse(*parsed->spec, *result->response,
                                  buffered);
      } catch (const std::exception &e) {
        std::cerr << "render: " << e.what() << '\n';
      }
      render::Flush(buf.str(), s.caps);
    }

    if (time_it) {
      const auto elapsed =
          std::chrono::steady_clock::now() - cmd_start;
      const auto total_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              elapsed)
              .count();
      const auto wire_ms = s.health.last_rtt.count();
      const bool color_ok =
          !s.caps.force_plain &&
          s.caps.colors != render::ColorDepth::None;
      const auto dim = color_ok
                           ? render::FgAnsi(theme.dim)
                           : std::string();
      constexpr const char *kReset = "\x1b[0m";
      std::cout << std::format(
          "{}  time: {}ms total, {}ms wire{}\n",
          dim, total_ms, wire_ms, kReset);
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
