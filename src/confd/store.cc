/// @file store.cc
/// @brief confd durable persistence implementation.
///
/// Single-file, line-based, atomically-replaced state. Each line is
/// `TAG field...`; for RUN / CVAL the value is the remainder of the
/// line (so a value may contain spaces, though CLI tokens do not). One
/// atomic rename swaps the whole state, so a crash mid-write never
/// leaves a torn file.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/store.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

auto StateFile(const std::string &dir) -> fs::path {
  return fs::path(dir) / "confd.state";
}

auto MakeError(StoreError code, std::string message) -> Error<StoreError> {
  return Error<StoreError>{code, std::move(message)};
}

// Parse "<int>" without throwing. Returns nullopt on any bad input.
auto ParseU64(const std::string &s) -> std::optional<std::uint64_t> {
  if (s.empty()) return std::nullopt;
  std::uint64_t out = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    out = out * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return out;
}

auto ParseI64(const std::string &s) -> std::optional<std::int64_t> {
  if (s.empty()) return std::nullopt;
  const bool neg = s[0] == '-';
  auto v = ParseU64(neg ? s.substr(1) : s);
  if (!v) return std::nullopt;
  return neg ? -static_cast<std::int64_t>(*v) : static_cast<std::int64_t>(*v);
}

}  // namespace

auto LoadState(const std::string &dir)
    -> std::expected<PersistentState, Error<StoreError>> {
  PersistentState state;
  const auto path = StateFile(dir);
  std::error_code ec;
  if (!fs::exists(path, ec)) return state;  // first boot

  std::ifstream f(path);
  if (!f.is_open()) {
    return std::unexpected(
        MakeError(StoreError::ReadFailed,
                  std::format("cannot open state file: {}", path.string())));
  }

  // Commits keyed by id while we accumulate their CVAL lines, plus an
  // order list so history keeps chronological order.
  std::unordered_map<CommitId, CommitRecord> commits;
  std::vector<CommitId> order;

  std::string line;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::istringstream iss(line);
    std::string tag;
    iss >> tag;
    if (tag == "NEXT_REV") {
      std::string n;
      iss >> n;
      auto v = ParseU64(n);
      if (!v) {
        return std::unexpected(
            MakeError(StoreError::ParseFailed, "bad NEXT_REV"));
      }
      state.next_rev = *v;
    } else if (tag == "PENDING") {
      std::string armed, rb, dl, pc;
      iss >> armed >> rb >> dl >> pc;
      auto rbv = ParseU64(rb);
      auto dlv = ParseI64(dl);
      auto pcv = ParseU64(pc);
      if (!rbv || !dlv || !pcv) {
        return std::unexpected(
            MakeError(StoreError::ParseFailed, "bad PENDING"));
      }
      state.pending.armed = armed == "1";
      state.pending.rollback_to = *rbv;
      state.pending.deadline_epoch_ms = *dlv;
      state.pending.pending_commit = *pcv;
    } else if (tag == "RUN") {
      std::string key;
      iss >> key;
      std::string value;
      std::getline(iss, value);
      if (!value.empty() && value[0] == ' ') value.erase(0, 1);
      state.running[key] = value;
    } else if (tag == "COMMIT") {
      std::string id, bid, author, ts;
      iss >> id >> bid >> author >> ts;
      auto idv = ParseU64(id);
      auto bidv = ParseU64(bid);
      if (!idv || !bidv) {
        return std::unexpected(
            MakeError(StoreError::ParseFailed, "bad COMMIT"));
      }
      CommitRecord rec;
      rec.id = *idv;
      rec.backend_id = *bidv;
      rec.author = author;
      rec.timestamp = ts;
      if (!commits.contains(*idv)) order.push_back(*idv);
      commits[*idv] = std::move(rec);
    } else if (tag == "CVAL") {
      std::string id, key;
      iss >> id >> key;
      auto idv = ParseU64(id);
      if (!idv) {
        return std::unexpected(MakeError(StoreError::ParseFailed, "bad CVAL"));
      }
      std::string value;
      std::getline(iss, value);
      if (!value.empty() && value[0] == ' ') value.erase(0, 1);
      commits[*idv].candidate.values[key] = value;
    }
    // Unknown tags are ignored for forward-compatibility.
  }

  for (const auto id : order) {
    state.history.push_back(std::move(commits[id]));
  }
  return state;
}

auto SaveState(const std::string &dir, const PersistentState &state)
    -> std::expected<void, Error<StoreError>> {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return std::unexpected(
        MakeError(StoreError::WriteFailed,
                  std::format("cannot create state dir: {}", ec.message())));
  }

  const auto final_path = StateFile(dir);
  const auto tmp_path =
      fs::path(dir) /
      std::format("confd.state.tmp.{}", static_cast<int>(::getpid()));
  {
    std::ofstream f(tmp_path, std::ios::trunc);
    if (!f.is_open()) {
      return std::unexpected(
          MakeError(StoreError::WriteFailed, "cannot open temp state file"));
    }
    f << std::format("NEXT_REV {}\n", state.next_rev);
    f << std::format("PENDING {} {} {} {}\n", state.pending.armed ? 1 : 0,
                     state.pending.rollback_to, state.pending.deadline_epoch_ms,
                     state.pending.pending_commit);
    for (const auto &[k, v] : state.running) {
      f << std::format("RUN {} {}\n", k, v);
    }
    for (const auto &c : state.history) {
      f << std::format("COMMIT {} {} {} {}\n", c.id, c.backend_id,
                       c.author.empty() ? "-" : c.author,
                       c.timestamp.empty() ? "-" : c.timestamp);
      for (const auto &[k, v] : c.candidate.values) {
        f << std::format("CVAL {} {} {}\n", c.id, k, v);
      }
    }
    f.flush();
    if (!f) {
      return std::unexpected(
          MakeError(StoreError::WriteFailed, "write to temp state failed"));
    }
  }

  fs::rename(tmp_path, final_path, ec);
  if (ec) {
    return std::unexpected(
        MakeError(StoreError::WriteFailed,
                  std::format("atomic rename failed: {}", ec.message())));
  }
  return {};
}

}  // namespace einheit::cli::confd
