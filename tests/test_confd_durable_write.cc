/// @file test_confd_durable_write.cc
/// @brief Atomic + durable replacement.
///
/// fsync cannot be observed from userspace without actually cutting the
/// power, so these tests pin the properties that CAN be checked here —
/// replacement semantics, no temp files left behind, failure leaving
/// the previous contents intact — while the power-cut soak
/// (s5/test/power_cycle.sh) covers the durability itself. That soak is
/// what found the missing fsync in the first place.
// Copyright (c) 2026 Einheit Networks

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/confd/durable_write.h"

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string &name)
      : path(fs::temp_directory_path() /
             ("einheit_durable_test_" + name + "_" +
              std::to_string(::getpid()))) {
    fs::remove_all(path);
  }
  ~TempDir() {
    fs::remove_all(path);
  }
};

auto Read(const fs::path &p) -> std::string {
  std::ifstream f(p);
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

auto CountFiles(const fs::path &dir) -> std::size_t {
  std::size_t n = 0;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    (void)e;
    ++n;
  }
  return n;
}

TEST(DurableWrite, CreatesTheFileAndItsParentDirectory) {
  TempDir dir("create");
  const auto path = dir.path / "nested" / "state.txt";
  ASSERT_TRUE(WriteFileDurably(path.string(), "hello\n").has_value());
  EXPECT_EQ(Read(path), "hello\n");
}

TEST(DurableWrite, ReplacesExistingContentWholesale) {
  TempDir dir("replace");
  const auto path = dir.path / "state.txt";
  ASSERT_TRUE(
      WriteFileDurably(path.string(), "first-and-longer\n").has_value());
  ASSERT_TRUE(WriteFileDurably(path.string(), "second\n").has_value());
  // No truncation leftovers from the longer previous contents.
  EXPECT_EQ(Read(path), "second\n");
}

TEST(DurableWrite, LeavesNoTempFileBehind) {
  TempDir dir("no_temp");
  const auto path = dir.path / "state.txt";
  ASSERT_TRUE(WriteFileDurably(path.string(), "x\n").has_value());
  ASSERT_TRUE(WriteFileDurably(path.string(), "y\n").has_value());
  // A store directory that accumulates .tmp.<pid> files would grow
  // without bound on a box that commits often.
  EXPECT_EQ(CountFiles(dir.path), 1u);
}

TEST(DurableWrite, HandlesEmptyContent) {
  TempDir dir("empty");
  const auto path = dir.path / "state.txt";
  ASSERT_TRUE(WriteFileDurably(path.string(), "seed\n").has_value());
  ASSERT_TRUE(WriteFileDurably(path.string(), "").has_value());
  EXPECT_EQ(Read(path), "");
}

TEST(DurableWrite, WritesContentLargerThanOnePage) {
  // The write loop has to handle a short write; a multi-page buffer is
  // the case where one can actually happen.
  TempDir dir("large");
  const auto path = dir.path / "state.txt";
  const std::string big(256 * 1024, 'k');
  ASSERT_TRUE(WriteFileDurably(path.string(), big).has_value());
  EXPECT_EQ(Read(path).size(), big.size());
}

TEST(DurableWrite, AFailedWriteLeavesThePreviousContentsIntact) {
  TempDir dir("failure");
  fs::create_directories(dir.path);
  const auto path = dir.path / "state.txt";
  ASSERT_TRUE(WriteFileDurably(path.string(), "good\n").has_value());

  // A path whose parent is a regular file, not a directory: the temp
  // create fails, and the original must be untouched.
  const auto blocked = dir.path / "state.txt" / "child";
  auto r = WriteFileDurably(blocked.string(), "bad\n");
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(Read(path), "good\n");
}

TEST(DurableWrite, ReportsAnUnwritableDirectory) {
  auto r = WriteFileDurably("/proc/definitely-not-writable/x", "no\n");
  ASSERT_FALSE(r.has_value());
  EXPECT_FALSE(r.error().message.empty());
}

}  // namespace
}  // namespace einheit::cli::confd
