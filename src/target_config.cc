/// @file target_config.cc
/// @brief Target config YAML parser + resolver.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/target_config.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace einheit::cli::target {
namespace {

auto MakeError(TargetError code, std::string message)
    -> Error<TargetError> {
  return Error<TargetError>{code, std::move(message)};
}

}  // namespace

auto LoadFromFile(const std::string &path)
    -> std::expected<TargetConfig, Error<TargetError>> {
  if (!std::filesystem::exists(path)) {
    return std::unexpected(MakeError(
        TargetError::NotFound,
        std::format("target config not found: {}", path)));
  }
  try {
    auto doc = YAML::LoadFile(path);
    TargetConfig out;

    if (doc["targets"] && doc["targets"].IsSequence()) {
      for (const auto &entry : doc["targets"]) {
        Target t;
        if (!entry["name"] || !entry["endpoint"]) {
          return std::unexpected(MakeError(
              TargetError::InvalidTarget,
              "target missing name/endpoint"));
        }
        t.name = entry["name"].as<std::string>();
        t.control_endpoint = entry["endpoint"].as<std::string>();
        if (entry["event_endpoint"]) {
          t.event_endpoint =
              entry["event_endpoint"].as<std::string>();
        }
        if (entry["server_key"]) {
          t.server_public_key =
              entry["server_key"].as<std::string>();
        }
        if (entry["client_key"]) {
          t.client_secret_key_path =
              entry["client_key"].as<std::string>();
        }
        out.targets.push_back(std::move(t));
      }
    }
    if (doc["default"]) {
      out.default_target = doc["default"].as<std::string>();
    }
    return out;
  } catch (const YAML::Exception &e) {
    return std::unexpected(
        MakeError(TargetError::ParseFailed, e.what()));
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(TargetError::ParseFailed, e.what()));
  }
}

auto LoadFromHome()
    -> std::expected<TargetConfig, Error<TargetError>> {
  const char *home = std::getenv("HOME");
  if (!home) {
    return std::unexpected(MakeError(
        TargetError::NotFound, "$HOME is not set"));
  }
  return LoadFromFile(std::format("{}/.einheit/config", home));
}

auto Resolve(const TargetConfig &cfg, const std::string &name)
    -> std::expected<const Target *, Error<TargetError>> {
  std::string want = name;
  if (want.empty()) {
    if (!cfg.default_target) {
      return std::unexpected(MakeError(
          TargetError::NoDefault,
          "no --target specified and no default in config"));
    }
    want = *cfg.default_target;
  }
  for (const auto &t : cfg.targets) {
    if (t.name == want) return &t;
  }
  return std::unexpected(MakeError(
      TargetError::UnknownTarget,
      std::format("unknown target: {}", want)));
}

}  // namespace einheit::cli::target
