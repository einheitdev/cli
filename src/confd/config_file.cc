/// @file config_file.cc
/// @brief Flat-kv config file implementation.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/config_file.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "einheit/cli/confd/durable_write.h"

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

auto MakeError(ConfigFileError code, std::string message)
    -> Error<ConfigFileError> {
  return Error<ConfigFileError>{code, std::move(message)};
}

}  // namespace

auto ValidConfigName(const std::string &name) -> bool {
  if (name.empty() || name.size() > 64) return false;
  if (name[0] == '.') return false;
  for (const char c : name) {
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 ||
                    c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

auto ConfigFilePath(const std::string &dir, const std::string &name)
    -> std::expected<std::string, Error<ConfigFileError>> {
  if (dir.empty()) {
    return std::unexpected(
        MakeError(ConfigFileError::NoConfigDir,
                  "this runtime has no config directory"));
  }
  if (!ValidConfigName(name)) {
    return std::unexpected(MakeError(
        ConfigFileError::BadName,
        std::format("invalid config name '{}': use 1-64 characters of "
                    "letters, digits, '.', '-' or '_'",
                    name)));
  }
  return (fs::path(dir) / (name + kConfigFileSuffix)).string();
}

auto WriteConfigFile(const std::string &path, const Config &config)
    -> std::expected<void, Error<ConfigFileError>> {
  const fs::path final_path(path);
  std::error_code ec;
  if (final_path.has_parent_path()) {
    fs::create_directories(final_path.parent_path(), ec);
    if (ec) {
      return std::unexpected(MakeError(
          ConfigFileError::WriteFailed,
          std::format("cannot create config dir: {}", ec.message())));
    }
  }

  std::vector<std::pair<std::string, std::string>> sorted(config.begin(),
                                                          config.end());
  std::sort(sorted.begin(), sorted.end());

  std::string body = std::string(kConfigFileHeader) + "\n";
  for (const auto &[k, v] : sorted) {
    body += std::format("{} {}\n", k, v);
  }
  // Durably: a backup an operator takes before a risky change is worth
  // nothing if a power cut can take it back with them.
  if (auto w = WriteFileDurably(final_path.string(), body); !w) {
    return std::unexpected(
        MakeError(ConfigFileError::WriteFailed, w.error().message));
  }
  return {};
}

auto ReadConfigFile(const std::string &path)
    -> std::expected<Config, Error<ConfigFileError>> {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return std::unexpected(MakeError(
        ConfigFileError::NotFound, std::format("no such file: {}", path)));
  }
  std::ifstream f(path);
  if (!f.is_open()) {
    return std::unexpected(MakeError(
        ConfigFileError::ReadFailed,
        std::format("cannot open config file: {}", path)));
  }
  Config out;
  std::string line;
  int lineno = 0;
  while (std::getline(f, line)) {
    ++lineno;
    // Trailing CR from a file edited on Windows would otherwise become
    // part of the last value.
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const auto sep = line.find(' ');
    if (sep == std::string::npos || sep == 0) {
      return std::unexpected(MakeError(
          ConfigFileError::ParseFailed,
          std::format("{}:{}: expected '<path> <value>'", path, lineno)));
    }
    out[line.substr(0, sep)] = line.substr(sep + 1);
  }
  return out;
}

auto ListConfigFiles(const std::string &dir) -> std::vector<std::string> {
  std::vector<std::string> names;
  if (dir.empty()) return names;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return names;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const auto file = entry.path().filename().string();
    const std::string suffix = kConfigFileSuffix;
    if (file.size() <= suffix.size()) continue;
    if (file.compare(file.size() - suffix.size(), suffix.size(), suffix) !=
        0) {
      continue;
    }
    names.push_back(file.substr(0, file.size() - suffix.size()));
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace einheit::cli::confd
