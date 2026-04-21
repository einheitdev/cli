/// @file history.cc
/// @brief Per-user command history (scaffold).
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/history.h"

#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>

namespace einheit::cli {
namespace {

auto MakeError(HistoryError code, std::string message)
    -> Error<HistoryError> {
  return Error<HistoryError>{code, std::move(message)};
}

auto HistoryPathFor(const std::string &base,
                    const std::string &user) -> std::string {
  return std::format("{}/{}/history", base, user);
}

}  // namespace

auto Load(const std::string &user, const std::string &base_path)
    -> std::expected<History, Error<HistoryError>> {
  try {
    History h;
    h.path = HistoryPathFor(base_path, user);
    std::ifstream f(h.path);
    std::string line;
    while (std::getline(f, line)) h.entries.push_back(line);
    return h;
  } catch (const std::exception &e) {
    return std::unexpected(MakeError(HistoryError::IoFailed, e.what()));
  }
}

auto Append(History &h, const std::string &entry)
    -> std::expected<void, Error<HistoryError>> {
  try {
    h.entries.push_back(entry);
    if (h.entries.size() > h.max_entries) {
      h.entries.erase(
          h.entries.begin(),
          h.entries.begin() + (h.entries.size() - h.max_entries));
    }
    namespace fs = std::filesystem;
    fs::create_directories(
        fs::path(h.path).parent_path());
    std::ofstream f(h.path, std::ios::app);
    f << entry << '\n';
    return {};
  } catch (const std::exception &e) {
    return std::unexpected(MakeError(HistoryError::IoFailed, e.what()));
  }
}

}  // namespace einheit::cli
