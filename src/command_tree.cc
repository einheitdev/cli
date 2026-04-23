/// @file command_tree.cc
/// @brief Command registration, parsing, and completion.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/command_tree.h"

#include <algorithm>
#include <format>
#include <sstream>
#include <string>
#include <utility>

#include "einheit/cli/fuzzy.h"
#include "einheit/cli/schema.h"

namespace einheit::cli {
namespace {

auto MakeError(CommandTreeError code, std::string message)
    -> Error<CommandTreeError> {
  return Error<CommandTreeError>{code, std::move(message)};
}

auto RoleAllows(RoleGate caller, RoleGate required) -> bool {
  if (required == RoleGate::AnyAuthenticated) return true;
  if (required == RoleGate::OperatorOrAdmin) {
    return caller == RoleGate::OperatorOrAdmin ||
           caller == RoleGate::AdminOnly;
  }
  return caller == RoleGate::AdminOnly;
}

}  // namespace

auto Register(CommandTree &tree, CommandSpec spec)
    -> std::expected<void, Error<CommandTreeError>> {
  auto [it, inserted] =
      tree.by_path.emplace(spec.path, std::move(spec));
  (void)it;
  if (!inserted) {
    return std::unexpected(MakeError(
        CommandTreeError::DuplicateRegistration,
        "command already registered"));
  }
  return {};
}

auto Parse(const CommandTree &tree,
           const std::vector<std::string> &tokens,
           RoleGate caller_role)
    -> std::expected<ParsedCommand, Error<CommandTreeError>> {
  // Match every registered path against the input, remembering both
  // exact and prefix-only hits. At the longest length, an exact
  // match wins outright; otherwise we require exactly one prefix
  // candidate (Junos-style unambiguous abbreviation). Ambiguous
  // prefixes surface as an "ambiguous" error listing alternatives.
  struct Match {
    const CommandSpec *spec;
    std::size_t len;
    bool all_exact;
  };
  std::vector<Match> matches;
  for (const auto &[path, spec] : tree.by_path) {
    std::istringstream iss(path);
    std::vector<std::string> parts;
    for (std::string p; iss >> p;) parts.push_back(std::move(p));
    if (parts.size() > tokens.size()) continue;
    bool ok = true;
    bool all_exact = true;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (parts[i] == tokens[i]) continue;
      if (parts[i].size() >= tokens[i].size() &&
          parts[i].compare(0, tokens[i].size(), tokens[i]) == 0) {
        all_exact = false;
        continue;
      }
      ok = false;
      break;
    }
    if (ok) matches.push_back({&spec, parts.size(), all_exact});
  }

  const CommandSpec *best = nullptr;
  std::size_t best_len = 0;
  if (!matches.empty()) {
    std::size_t max_len = 0;
    for (const auto &m : matches) max_len = std::max(max_len, m.len);

    // At the longest length, find the exact hit or a unique prefix.
    const CommandSpec *exact = nullptr;
    std::vector<const CommandSpec *> prefix_only;
    for (const auto &m : matches) {
      if (m.len != max_len) continue;
      if (m.all_exact && !exact) exact = m.spec;
      if (!m.all_exact) prefix_only.push_back(m.spec);
    }
    if (exact) {
      best = exact;
      best_len = max_len;
    } else if (prefix_only.size() == 1) {
      best = prefix_only.front();
      best_len = max_len;
    } else if (prefix_only.size() > 1) {
      std::string options;
      for (std::size_t i = 0; i < prefix_only.size(); ++i) {
        if (i > 0) options += ", ";
        options += prefix_only[i]->path;
      }
      return std::unexpected(MakeError(
          CommandTreeError::UnknownCommand,
          std::format("ambiguous — did you mean: {}", options)));
    }
  }

  if (!best) {
    // Fuzzy-match the first token against known verbs for a hint.
    std::string message = "no matching command";
    if (!tokens.empty()) {
      std::vector<std::string> verbs;
      verbs.reserve(tree.by_path.size());
      for (const auto &[path, _] : tree.by_path) {
        std::istringstream iss(path);
        std::string first;
        if (iss >> first) verbs.push_back(std::move(first));
      }
      std::sort(verbs.begin(), verbs.end());
      verbs.erase(std::unique(verbs.begin(), verbs.end()), verbs.end());
      auto suggestions = fuzzy::Suggest(tokens[0], verbs);
      if (!suggestions.empty()) {
        message = std::format("{} — did you mean '{}'?", message,
                              suggestions.front());
      }
    }
    return std::unexpected(
        MakeError(CommandTreeError::UnknownCommand, std::move(message)));
  }
  if (!RoleAllows(caller_role, best->role)) {
    return std::unexpected(MakeError(
        CommandTreeError::NotAuthorised, "role does not allow"));
  }

  ParsedCommand out;
  out.spec = best;
  for (std::size_t i = best_len; i < tokens.size(); ++i) {
    out.args.push_back(tokens[i]);
  }
  for (std::size_t i = 0; i < best->args.size(); ++i) {
    if (i >= out.args.size() && best->args[i].required) {
      return std::unexpected(MakeError(
          CommandTreeError::MissingArgument, best->args[i].name));
    }
  }
  return out;
}

namespace {
auto RoleLabel(RoleGate role) -> const char * {
  switch (role) {
    case RoleGate::AdminOnly:         return "admin";
    case RoleGate::OperatorOrAdmin:   return "operator";
    case RoleGate::AnyAuthenticated:
    default:                          return "any";
  }
}
}  // namespace

auto FormatHelpIndex(const CommandTree &tree) -> std::string {
  std::vector<const CommandSpec *> sorted;
  sorted.reserve(tree.by_path.size());
  for (const auto &[_, spec] : tree.by_path) sorted.push_back(&spec);
  std::sort(sorted.begin(), sorted.end(),
            [](const auto *a, const auto *b) {
              return a->path < b->path;
            });

  std::size_t width = 0;
  for (const auto *s : sorted) width = std::max(width, s->path.size());

  std::string out;
  for (const auto *s : sorted) {
    out += std::format("  {:<{}}  — {}\n", s->path, width, s->help);
  }
  return out;
}

auto FormatCommandHelp(const CommandSpec &spec) -> std::string {
  std::string out = std::format("{}\n", spec.path);
  if (!spec.help.empty()) {
    out += std::format("  {}\n\n", spec.help);
  }
  out += std::format("  role: {}\n", RoleLabel(spec.role));
  if (spec.requires_session) {
    out += "  requires: configure session\n";
  }
  if (!spec.args.empty()) {
    out += "  args:\n";
    for (const auto &a : spec.args) {
      out += std::format("    <{}> {}", a.name,
                         a.required ? "(required)" : "(optional)");
      if (!a.type_ref.empty()) out += std::format(" : {}", a.type_ref);
      if (!a.help.empty()) out += std::format(" — {}", a.help);
      out += '\n';
    }
  }
  if (!spec.flags.empty()) {
    out += "  flags:\n";
    for (const auto &f : spec.flags) {
      out += std::format("    --{}\n", f);
    }
  }
  return out;
}

auto BuildHelpIndex(const CommandTree &tree) -> render::Table {
  std::vector<const CommandSpec *> sorted;
  sorted.reserve(tree.by_path.size());
  for (const auto &[_, spec] : tree.by_path) sorted.push_back(&spec);
  std::sort(sorted.begin(), sorted.end(),
            [](const auto *a, const auto *b) {
              return a->path < b->path;
            });

  render::Table t;
  AddColumn(t, "command", render::Align::Left,
            render::Priority::High);
  AddColumn(t, "role", render::Align::Left,
            render::Priority::Medium);
  AddColumn(t, "description", render::Align::Left,
            render::Priority::High);

  for (const auto *s : sorted) {
    render::Semantic role_sem = render::Semantic::Dim;
    if (s->role == RoleGate::AdminOnly) {
      role_sem = render::Semantic::Warn;
    } else if (s->role == RoleGate::OperatorOrAdmin) {
      role_sem = render::Semantic::Info;
    }
    AddRow(t, {
        render::Cell{s->path, render::Semantic::Emphasis},
        render::Cell{RoleLabel(s->role), role_sem},
        render::Cell{s->help},
    });
  }
  return t;
}

auto BuildCommandHelp(const CommandSpec &spec) -> render::Table {
  render::Table t;
  AddColumn(t, "field", render::Align::Left,
            render::Priority::High);
  AddColumn(t, "value", render::Align::Left,
            render::Priority::High);

  AddRow(t, {render::Cell{"command", render::Semantic::Emphasis},
             render::Cell{spec.path, render::Semantic::Info}});
  if (!spec.help.empty()) {
    AddRow(t, {render::Cell{"summary", render::Semantic::Emphasis},
               render::Cell{spec.help}});
  }
  render::Semantic role_sem = render::Semantic::Dim;
  if (spec.role == RoleGate::AdminOnly) {
    role_sem = render::Semantic::Warn;
  } else if (spec.role == RoleGate::OperatorOrAdmin) {
    role_sem = render::Semantic::Info;
  }
  AddRow(t, {render::Cell{"role", render::Semantic::Emphasis},
             render::Cell{RoleLabel(spec.role), role_sem}});
  if (spec.requires_session) {
    AddRow(t, {render::Cell{"requires", render::Semantic::Emphasis},
               render::Cell{"configure session",
                            render::Semantic::Warn}});
  }
  for (std::size_t i = 0; i < spec.args.size(); ++i) {
    const auto &a = spec.args[i];
    std::string value = std::format(
        "<{}> {}", a.name,
        a.required ? "(required)" : "(optional)");
    if (!a.type_ref.empty()) value += std::format(" : {}", a.type_ref);
    if (!a.help.empty()) value += std::format(" — {}", a.help);
    AddRow(t, {render::Cell{std::format("arg[{}]", i),
                            render::Semantic::Emphasis},
               render::Cell{value}});
  }
  for (std::size_t i = 0; i < spec.flags.size(); ++i) {
    AddRow(t, {
        render::Cell{std::format("flag[{}]", i),
                     render::Semantic::Emphasis},
        render::Cell{std::format("--{}", spec.flags[i]),
                     render::Semantic::Info},
    });
  }
  return t;
}

auto SuggestCompletions(const CommandTree &tree,
                        const std::vector<std::string> &preceding,
                        const std::string &partial)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto &[path, spec] : tree.by_path) {
    std::istringstream iss(path);
    std::vector<std::string> parts;
    for (std::string p; iss >> p;) parts.push_back(std::move(p));
    if (parts.size() <= preceding.size()) continue;
    bool prefix_match = true;
    for (std::size_t i = 0; i < preceding.size(); ++i) {
      if (parts[i] != preceding[i]) { prefix_match = false; break; }
    }
    if (!prefix_match) continue;
    const auto &next = parts[preceding.size()];
    if (next.rfind(partial, 0) == 0) {
      out.push_back(next);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

auto SuggestCompletions(const CommandTree &tree,
                        const schema::Schema &schema,
                        const std::vector<std::string> &preceding,
                        const std::string &partial)
    -> std::vector<std::string> {
  // `set <path> [value]` and `delete <path>` route the path argument
  // through the schema field tree.
  const bool is_set_path =
      preceding.size() == 1 && preceding[0] == "set";
  const bool is_delete_path =
      preceding.size() == 1 && preceding[0] == "delete";
  if (is_set_path || is_delete_path) {
    return schema::Completions(schema, partial);
  }
  // `set <path> <value>` — value completion pulls enum members /
  // booleans out of the schema so the operator doesn't have to
  // remember legal values. Non-finite types (string, integer,
  // cidr, custom) fall through to an empty list.
  const bool is_set_value =
      preceding.size() == 2 && preceding[0] == "set";
  if (is_set_value) {
    return schema::ValueCompletions(schema, preceding[1],
                                     partial);
  }
  return SuggestCompletions(tree, preceding, partial);
}

}  // namespace einheit::cli
