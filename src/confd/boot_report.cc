/// @file boot_report.cc
/// @brief Boot-report persistence, in the store's line-based format.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/boot_report.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "einheit/cli/confd/durable_write.h"

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

auto ReportFile(const std::string &dir) -> fs::path {
  return fs::path(dir) / "boot.report";
}

auto MakeError(BootReportError code, std::string message)
    -> Error<BootReportError> {
  return Error<BootReportError>{code, std::move(message)};
}

auto ParseU64(const std::string &s) -> std::optional<std::uint64_t> {
  if (s.empty()) return std::nullopt;
  std::uint64_t out = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    out = out * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return out;
}

// Steps carry free text, so their fields are separated by tabs rather
// than spaces — a detail like "enslaving lan1 to br0 failed" must not
// be re-split on read.
constexpr char kStepSep = '\t';

}  // namespace

auto CurrentBootId() -> std::string {
  std::ifstream f("/proc/sys/kernel/random/boot_id");
  if (!f.is_open()) return {};
  std::string id;
  std::getline(f, id);
  // Guard against anything that is not a plain identifier; this value
  // ends up in rendered output.
  for (const char c : id) {
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 ||
                    c == '-';
    if (!ok) return {};
  }
  return id;
}

auto IsFromCurrentBoot(const BootReport &report,
                       const std::string &current_boot_id) -> bool {
  if (report.boot_id.empty() || current_boot_id.empty()) return false;
  return report.boot_id == current_boot_id;
}

auto LoadBootReport(const std::string &dir)
    -> std::expected<std::optional<BootReport>, Error<BootReportError>> {
  const auto path = ReportFile(dir);
  std::error_code ec;
  if (!fs::exists(path, ec)) return std::nullopt;
  std::ifstream f(path);
  if (!f.is_open()) {
    return std::unexpected(MakeError(
        BootReportError::ReadFailed,
        std::format("cannot open boot report: {}", path.string())));
  }

  BootReport r;
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::istringstream iss(line);
    std::string tag;
    iss >> tag;
    if (tag == "STEP") {
      // STEP <ok>\t<duration_ms>\t<name>\t<detail>
      std::string rest;
      std::getline(iss, rest);
      if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
      std::istringstream fields(rest);
      std::string ok_s, dur_s;
      BootStep step;
      std::getline(fields, ok_s, kStepSep);
      std::getline(fields, dur_s, kStepSep);
      std::getline(fields, step.name, kStepSep);
      std::getline(fields, step.detail);
      step.ok = ok_s == "1";
      step.duration_ms =
          static_cast<std::int64_t>(ParseU64(dur_s).value_or(0));
      r.steps.push_back(std::move(step));
      continue;
    }
    std::string value;
    std::getline(iss, value);
    if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    if (tag == "BOOT_ID") {
      r.boot_id = value;
    } else if (tag == "AT") {
      r.timestamp = value;
    } else if (tag == "OK") {
      r.ok = value == "1";
    } else if (tag == "REVISION") {
      r.applied_revision = ParseU64(value).value_or(0);
    } else if (tag == "PATHS") {
      r.paths = static_cast<std::size_t>(ParseU64(value).value_or(0));
    } else if (tag == "SEEDED_FACTORY") {
      r.seeded_factory = value == "1";
    } else if (tag == "REVERTED_PENDING") {
      r.reverted_pending = value == "1";
    } else if (tag == "RECONCILE_ADDED") {
      r.reconcile_added =
          static_cast<std::size_t>(ParseU64(value).value_or(0));
    } else if (tag == "RECONCILE_CONFLICTS") {
      r.reconcile_conflicts =
          static_cast<std::size_t>(ParseU64(value).value_or(0));
    } else if (tag == "DURATION_MS") {
      r.duration_ms =
          static_cast<std::int64_t>(ParseU64(value).value_or(0));
    }
    // Unknown tags are ignored for forward-compatibility.
  }
  return r;
}

auto SaveBootReport(const std::string &dir, const BootReport &report)
    -> std::expected<void, Error<BootReportError>> {
  std::string body;
  body += std::format("BOOT_ID {}\n", report.boot_id);
  body += std::format("AT {}\n", report.timestamp);
  body += std::format("OK {}\n", report.ok ? 1 : 0);
  body += std::format("REVISION {}\n", report.applied_revision);
  body += std::format("PATHS {}\n", report.paths);
  body += std::format("SEEDED_FACTORY {}\n", report.seeded_factory ? 1 : 0);
  body +=
      std::format("REVERTED_PENDING {}\n", report.reverted_pending ? 1 : 0);
  body += std::format("RECONCILE_ADDED {}\n", report.reconcile_added);
  body +=
      std::format("RECONCILE_CONFLICTS {}\n", report.reconcile_conflicts);
  body += std::format("DURATION_MS {}\n", report.duration_ms);
  for (const auto &s : report.steps) {
    body += std::format("STEP {}{}{}{}{}{}{}\n", s.ok ? 1 : 0, kStepSep,
                        s.duration_ms, kStepSep, s.name, kStepSep,
                        s.detail);
  }
  if (auto w = WriteFileDurably(ReportFile(dir).string(), body); !w) {
    return std::unexpected(
        MakeError(BootReportError::WriteFailed, w.error().message));
  }
  return {};
}

}  // namespace einheit::cli::confd
