/// @file durable_write.cc
/// @brief Atomic + durable file replacement over POSIX fsync.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/durable_write.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

auto MakeError(DurableWriteError code, std::string message)
    -> Error<DurableWriteError> {
  return Error<DurableWriteError>{code, std::move(message)};
}

// Write the whole buffer, retrying short writes. write(2) is allowed to
// write less than asked even to a regular file.
auto WriteAll(int fd, const std::string &content) -> bool {
  std::size_t written = 0;
  while (written < content.size()) {
    const auto n = ::write(fd, content.data() + written,
                           content.size() - written);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    written += static_cast<std::size_t>(n);
  }
  return true;
}

// fsync the directory holding `path`, which is what makes the rename
// itself durable. Without this the file contents survive but the
// directory entry can still point at the old inode after power loss.
auto SyncParentDir(const fs::path &path) -> bool {
  const auto dir = path.has_parent_path() ? path.parent_path()
                                          : fs::path(".");
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

}  // namespace

auto WriteFileDurably(const std::string &path, const std::string &content)
    -> std::expected<void, Error<DurableWriteError>> {
  const fs::path final_path(path);
  std::error_code ec;
  if (final_path.has_parent_path()) {
    fs::create_directories(final_path.parent_path(), ec);
    if (ec) {
      return std::unexpected(MakeError(
          DurableWriteError::DirFailed,
          std::format("cannot create directory for {}: {}", path,
                      ec.message())));
    }
  }

  const fs::path tmp = std::format(
      "{}.tmp.{}", path, static_cast<std::int64_t>(::getpid()));
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return std::unexpected(
        MakeError(DurableWriteError::WriteFailed,
                  std::format("cannot open {}", tmp.string())));
  }
  if (!WriteAll(fd, content)) {
    ::close(fd);
    fs::remove(tmp, ec);
    return std::unexpected(
        MakeError(DurableWriteError::WriteFailed,
                  std::format("write to {} failed", tmp.string())));
  }
  // Flush the data BEFORE the rename. A rename that lands before its
  // contents do would publish an empty or partial file.
  if (::fsync(fd) != 0) {
    ::close(fd);
    fs::remove(tmp, ec);
    return std::unexpected(
        MakeError(DurableWriteError::SyncFailed,
                  std::format("fsync of {} failed", tmp.string())));
  }
  if (::close(fd) != 0) {
    fs::remove(tmp, ec);
    return std::unexpected(
        MakeError(DurableWriteError::SyncFailed,
                  std::format("close of {} failed", tmp.string())));
  }

  fs::rename(tmp, final_path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return std::unexpected(
        MakeError(DurableWriteError::RenameFailed,
                  std::format("rename to {} failed: {}", path,
                              ec.message())));
  }
  if (!SyncParentDir(final_path)) {
    // The contents are on disk and the rename happened; only its
    // durability is unproven. Report it rather than pretend, so a
    // caller can decide whether to trust the write.
    return std::unexpected(MakeError(
        DurableWriteError::SyncFailed,
        std::format("fsync of the directory holding {} failed", path)));
  }
  return {};
}

}  // namespace einheit::cli::confd
