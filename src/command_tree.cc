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
  // Longest-prefix match against the path token set.
  const CommandSpec *best = nullptr;
  std::size_t best_len = 0;
  for (const auto &[path, spec] : tree.by_path) {
    std::istringstream iss(path);
    std::vector<std::string> parts;
    for (std::string p; iss >> p;) parts.push_back(std::move(p));
    if (parts.size() > tokens.size()) continue;
    bool match = true;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (parts[i] != tokens[i]) { match = false; break; }
    }
    if (match && parts.size() > best_len) {
      best = &spec;
      best_len = parts.size();
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
  return SuggestCompletions(tree, preceding, partial);
}

}  // namespace einheit::cli
