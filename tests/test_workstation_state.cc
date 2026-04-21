/// @file test_workstation_state.cc
/// @brief Round-trip + edge-case tests for per-user state.
// Copyright (c) 2026 Einheit Networks

#include <filesystem>
#include <format>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "einheit/cli/workstation_state.h"

namespace einheit::cli::workstation {
namespace {

class TmpDir {
 public:
  TmpDir() {
    path_ =
        std::filesystem::temp_directory_path() /
        std::format("einheit_state_{}_{}",
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

TEST(WorkstationState, MissingFileLoadsEmpty) {
  TmpDir d;
  auto s = Load(d.Path() + "/state");
  ASSERT_TRUE(s.has_value());
  EXPECT_FALSE(s->active_target.has_value());
}

TEST(WorkstationState, SaveThenLoadRoundTrip) {
  TmpDir d;
  State in;
  in.active_target = "berlin";
  ASSERT_TRUE(Save(d.Path() + "/state", in).has_value());

  auto out = Load(d.Path() + "/state");
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->active_target.value_or(""), "berlin");
}

TEST(WorkstationState, OverwritesPriorValue) {
  TmpDir d;
  State first;
  first.active_target = "berlin";
  ASSERT_TRUE(Save(d.Path() + "/state", first).has_value());

  State second;
  second.active_target = "munich";
  ASSERT_TRUE(Save(d.Path() + "/state", second).has_value());

  auto loaded = Load(d.Path() + "/state");
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->active_target.value_or(""), "munich");
}

TEST(WorkstationState, CreatesParentDirectories) {
  TmpDir d;
  State s;
  s.active_target = "paris";
  const auto path = d.Path() + "/nested/parent/state";
  ASSERT_TRUE(Save(path, s).has_value());
  EXPECT_TRUE(std::filesystem::exists(path));
}

}  // namespace einheit::cli::workstation
