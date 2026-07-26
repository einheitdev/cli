/// @file edit_lock.cc
/// @brief Durable configure-mode edit lock implementation.
///
/// One `configure.lock` file next to the confd state, written with the
/// same `TAG value` line shape as the store so it is inspectable. The
/// uncontended acquire is a single O_CREAT|O_EXCL create, which is the
/// atomic step that makes two processes racing for configure mode
/// resolve to exactly one winner.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/edit_lock.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

auto LockFile(const std::string &dir) -> fs::path {
  return fs::path(dir) / "configure.lock";
}

auto MakeError(LockError code, std::string message) -> Error<LockError> {
  return Error<LockError>{code, std::move(message)};
}

auto ParseI64(const std::string &s) -> std::optional<std::int64_t> {
  if (s.empty()) return std::nullopt;
  std::int64_t out = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    out = out * 10 + static_cast<std::int64_t>(c - '0');
  }
  return out;
}

auto Serialize(const EditLock &lock) -> std::string {
  // A holder / session / timestamp is a single token by construction
  // (user names and confd session ids never contain spaces), so the
  // line format needs no quoting.
  return std::format("HOLDER {}\nSESSION {}\nPID {}\nAT {}\n",
                     lock.holder.empty() ? "-" : lock.holder,
                     lock.session_id.empty() ? "-" : lock.session_id,
                     lock.pid, lock.acquired_at.empty()
                                   ? "-"
                                   : lock.acquired_at);
}

auto Deserialize(std::istream &in)
    -> std::expected<EditLock, Error<LockError>> {
  EditLock lock;
  bool saw_session = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream iss(line);
    std::string tag;
    std::string value;
    iss >> tag >> value;
    if (tag == "HOLDER") {
      lock.holder = value == "-" ? std::string() : value;
    } else if (tag == "SESSION") {
      lock.session_id = value == "-" ? std::string() : value;
      saw_session = true;
    } else if (tag == "PID") {
      const auto pid = ParseI64(value);
      if (!pid) {
        return std::unexpected(
            MakeError(LockError::ParseFailed, "bad PID in lock file"));
      }
      lock.pid = *pid;
    } else if (tag == "AT") {
      lock.acquired_at = value == "-" ? std::string() : value;
    }
    // Unknown tags are ignored for forward-compatibility.
  }
  if (!saw_session) {
    return std::unexpected(
        MakeError(LockError::ParseFailed, "lock file has no SESSION"));
  }
  return lock;
}

// Replace the lock file's contents atomically (write temp + rename).
// Used when displacing a stale or force-stolen holder — the create
// race is already lost at that point, so exclusivity comes from the
// caller's decision, not from O_EXCL.
auto Overwrite(const fs::path &path, const EditLock &lock)
    -> std::expected<void, Error<LockError>> {
  const fs::path tmp = std::format(
      "{}.tmp.{}", path.string(),
      static_cast<std::int64_t>(::getpid()));
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f.is_open()) {
      return std::unexpected(MakeError(LockError::WriteFailed,
                                       "cannot open temp lock file"));
    }
    f << Serialize(lock);
    f.flush();
    if (!f) {
      return std::unexpected(MakeError(LockError::WriteFailed,
                                       "write to temp lock failed"));
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return std::unexpected(MakeError(
        LockError::WriteFailed,
        std::format("lock rename failed: {}", ec.message())));
  }
  return {};
}

// Kept short on purpose: the CLI gives an error message one line and
// truncates the rest, and pid / acquisition time are already available
// from `show status`. The holder and the session are what the operator
// needs to see here.
auto HeldMessage(const EditLock &lock) -> std::string {
  return std::format(
      "configure mode is held by {} (session {})",
      lock.holder.empty() ? "another session" : lock.holder,
      lock.session_id);
}

}  // namespace

auto EditLockHolderAlive(const EditLock &lock) -> bool {
  // An unknown pid cannot be probed; treat it as live so the holder is
  // only ever displaced deliberately.
  if (lock.pid <= 0) return true;
  if (lock.pid == static_cast<std::int64_t>(::getpid())) return true;
  if (::kill(static_cast<pid_t>(lock.pid), 0) == 0) return true;
  // EPERM means the process exists but belongs to another user.
  return errno == EPERM;
}

auto ReadEditLock(const std::string &dir)
    -> std::expected<std::optional<EditLock>, Error<LockError>> {
  const auto path = LockFile(dir);
  std::error_code ec;
  if (!fs::exists(path, ec)) return std::nullopt;
  std::ifstream f(path);
  if (!f.is_open()) {
    return std::unexpected(MakeError(
        LockError::ReadFailed,
        std::format("cannot open lock file: {}", path.string())));
  }
  auto lock = Deserialize(f);
  if (!lock) return std::unexpected(lock.error());
  return *lock;
}

auto AcquireEditLock(const std::string &dir, const EditLock &want,
                     bool force)
    -> std::expected<AcquireOutcome, Error<LockError>> {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return std::unexpected(MakeError(
        LockError::WriteFailed,
        std::format("cannot create state dir: {}", ec.message())));
  }

  const auto path = LockFile(dir);
  AcquireOutcome out;

  // Uncontended path: whoever wins this create owns configure mode.
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd >= 0) {
    const auto body = Serialize(want);
    const auto written =
        ::write(fd, body.data(), static_cast<std::size_t>(body.size()));
    ::close(fd);
    if (written != static_cast<ssize_t>(body.size())) {
      fs::remove(path, ec);
      return std::unexpected(
          MakeError(LockError::WriteFailed, "short write to lock file"));
    }
    return out;
  }
  if (errno != EEXIST) {
    return std::unexpected(MakeError(
        LockError::WriteFailed,
        std::format("cannot create lock file: {}", path.string())));
  }

  // Contended: decide whether the existing holder may be displaced. A
  // lock file we cannot parse is treated as stale rather than as a
  // permanent lockout — it is a cache of who is editing, not a
  // security boundary.
  auto existing = ReadEditLock(dir);
  if (!existing || !existing->has_value()) {
    if (auto w = Overwrite(path, want); !w) {
      return std::unexpected(w.error());
    }
    return out;
  }
  const EditLock &held = **existing;

  if (held.session_id == want.session_id) {
    // Our own session refreshing itself.
    if (auto w = Overwrite(path, want); !w) {
      return std::unexpected(w.error());
    }
    return out;
  }
  const bool alive = EditLockHolderAlive(held);
  if (alive && !force) {
    return std::unexpected(MakeError(LockError::Held, HeldMessage(held)));
  }
  if (alive) {
    out.stolen_from = held;
  } else {
    out.reclaimed_from = held;
  }
  if (auto w = Overwrite(path, want); !w) {
    return std::unexpected(w.error());
  }
  return out;
}

auto ReleaseEditLock(const std::string &dir,
                     const std::string &session_id) -> void {
  auto held = ReadEditLock(dir);
  if (!held || !held->has_value()) return;
  if ((*held)->session_id != session_id) return;
  std::error_code ec;
  fs::remove(LockFile(dir), ec);
}

}  // namespace einheit::cli::confd
