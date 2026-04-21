/// @file workstation_state.cc
/// @brief Per-user state persistence.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/workstation_state.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace einheit::cli::workstation {
namespace {

auto MakeError(StateError code, std::string message)
    -> Error<StateError> {
  return Error<StateError>{code, std::move(message)};
}

}  // namespace

auto DefaultPath() -> std::string {
  const char *home = std::getenv("HOME");
  if (!home) return {};
  return std::format("{}/.einheit/state", home);
}

auto Load(const std::string &path)
    -> std::expected<State, Error<StateError>> {
  State s;
  if (path.empty() || !std::filesystem::exists(path)) return s;
  try {
    auto doc = YAML::LoadFile(path);
    if (doc["active_target"]) {
      s.active_target = doc["active_target"].as<std::string>();
    }
    if (doc["active_theme"]) {
      s.active_theme = doc["active_theme"].as<std::string>();
    }
    return s;
  } catch (const YAML::Exception &e) {
    return std::unexpected(
        MakeError(StateError::ParseFailed, e.what()));
  } catch (const std::exception &e) {
    return std::unexpected(MakeError(StateError::IoFailed, e.what()));
  }
}

auto Save(const std::string &path, const State &s)
    -> std::expected<void, Error<StateError>> {
  if (path.empty()) {
    return std::unexpected(MakeError(
        StateError::IoFailed, "state path is empty ($HOME unset?)"));
  }
  try {
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    YAML::Node doc;
    if (s.active_target) {
      doc["active_target"] = *s.active_target;
    }
    if (s.active_theme) {
      doc["active_theme"] = *s.active_theme;
    }
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
      return std::unexpected(MakeError(
          StateError::IoFailed,
          std::format("could not open for write: {}", path)));
    }
    f << doc;
    return {};
  } catch (const std::exception &e) {
    return std::unexpected(MakeError(StateError::IoFailed, e.what()));
  }
}

}  // namespace einheit::cli::workstation
