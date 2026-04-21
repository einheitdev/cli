/// @file aliases.cc
/// @brief Alias loading (legacy + YAML) and expansion.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/aliases.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace einheit::cli {
namespace {

auto MakeError(AliasError code, std::string message)
    -> Error<AliasError> {
  return Error<AliasError>{code, std::move(message)};
}

auto AliasPathFor(const std::string &base,
                  const std::string &user) -> std::string {
  return std::format("{}/{}/aliases", base, user);
}

// Recursively load a YAML alias file. `seen` prevents include cycles.
auto LoadYamlInto(const std::string &path, Aliases &out,
                  std::unordered_set<std::string> &seen)
    -> std::expected<void, Error<AliasError>> {
  namespace fs = std::filesystem;
  const auto canonical = fs::weakly_canonical(path).string();
  if (seen.contains(canonical)) return {};
  seen.insert(canonical);
  if (!fs::exists(path)) {
    return std::unexpected(MakeError(
        AliasError::NotAccessible,
        std::format("alias file not found: {}", path)));
  }

  YAML::Node doc;
  try {
    doc = YAML::LoadFile(path);
  } catch (const YAML::Exception &e) {
    return std::unexpected(
        MakeError(AliasError::Malformed, e.what()));
  }

  // Includes are merged first so later definitions override.
  if (doc["include"] && doc["include"].IsSequence()) {
    for (const auto &inc : doc["include"]) {
      const auto inc_path = inc.as<std::string>();
      if (auto r = LoadYamlInto(inc_path, out, seen); !r) {
        return r;
      }
    }
  }

  if (doc["aliases"] && doc["aliases"].IsMap()) {
    for (const auto &kv : doc["aliases"]) {
      const auto name = kv.first.as<std::string>();
      const auto &val = kv.second;
      if (val.IsScalar()) {
        out.table[name] = val.as<std::string>();
      } else if (val.IsMap()) {
        if (val["expansion"]) {
          out.table[name] = val["expansion"].as<std::string>();
        }
        if (val["help"]) {
          out.help[name] = val["help"].as<std::string>();
        }
      }
    }
  }
  return {};
}

}  // namespace

auto LoadAliases(const std::string &user,
                 const std::string &base_path)
    -> std::expected<Aliases, Error<AliasError>> {
  try {
    Aliases a;
    a.path = AliasPathFor(base_path, user);
    std::ifstream f(a.path);
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      a.table.emplace(line.substr(0, eq), line.substr(eq + 1));
    }
    return a;
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(AliasError::NotAccessible, e.what()));
  }
}

auto LoadAliasesYaml(const std::string &path)
    -> std::expected<Aliases, Error<AliasError>> {
  Aliases out;
  out.path = path;
  std::unordered_set<std::string> seen;
  if (auto r = LoadYamlInto(path, out, seen); !r) {
    return std::unexpected(r.error());
  }
  return out;
}

auto DefaultYamlPath() -> std::string {
  const char *home = std::getenv("HOME");
  if (!home) return {};
  return std::format("{}/.einheit/aliases.yaml", home);
}

auto MergeAliases(Aliases &base, const Aliases &other) -> void {
  for (const auto &[k, v] : other.table) base.table[k] = v;
  for (const auto &[k, v] : other.help) base.help[k] = v;
}

auto Expand(const Aliases &a, const std::string &line) -> std::string {
  std::istringstream iss(line);
  std::string first;
  if (!(iss >> first)) return line;
  auto it = a.table.find(first);
  if (it == a.table.end()) return line;
  std::string rest;
  std::getline(iss, rest);
  return it->second + rest;
}

}  // namespace einheit::cli
