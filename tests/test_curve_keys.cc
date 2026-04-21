/// @file test_curve_keys.cc
/// @brief Generation + round-trip tests for CurveZMQ keypairs.
// Copyright (c) 2026 Einheit Networks

#include <sys/stat.h>

#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "einheit/cli/curve_keys.h"

namespace einheit::cli::curve {
namespace {

class TmpDir {
 public:
  TmpDir() {
    path_ =
        std::filesystem::temp_directory_path() /
        std::format("einheit_curve_{}_{}",
                    static_cast<int>(::getpid()), counter_++);
    std::filesystem::create_directories(path_);
  }
  ~TmpDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  auto Path() const -> std::string { return path_.string(); }

 private:
  static inline int counter_ = 0;
  std::filesystem::path path_;
};

}  // namespace

TEST(CurveKeys, GeneratesZ85Pair) {
  auto kp = Generate();
  ASSERT_TRUE(kp.has_value()) << kp.error().message;
  EXPECT_EQ(kp->public_key.size(), 40u);
  EXPECT_EQ(kp->secret_key.size(), 40u);
  EXPECT_NE(kp->public_key, kp->secret_key);
}

TEST(CurveKeys, WriteThenReadRoundTrip) {
  TmpDir d;
  auto kp = Generate();
  ASSERT_TRUE(kp.has_value());
  ASSERT_TRUE(WriteToDisk(d.Path(), "karl", *kp).has_value());

  auto loaded = ReadFromDisk(d.Path(), "karl");
  ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
  EXPECT_EQ(loaded->public_key, kp->public_key);
  EXPECT_EQ(loaded->secret_key, kp->secret_key);
}

TEST(CurveKeys, SecretFileIsMode0600) {
  TmpDir d;
  auto kp = Generate();
  ASSERT_TRUE(kp.has_value());
  ASSERT_TRUE(WriteToDisk(d.Path(), "karl", *kp).has_value());

  struct stat st{};
  const auto path = std::format("{}/karl.secret", d.Path());
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST(CurveKeys, DifferentGenerationsDiffer) {
  auto a = Generate();
  auto b = Generate();
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_NE(a->public_key, b->public_key);
  EXPECT_NE(a->secret_key, b->secret_key);
}

TEST(CurveKeys, ReadFailsOnMissing) {
  TmpDir d;
  auto r = ReadFromDisk(d.Path(), "absent");
  EXPECT_FALSE(r.has_value());
}

}  // namespace einheit::cli::curve
