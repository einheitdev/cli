/// @file test_theme.cc
/// @brief Theme loading + light-terminal detection tests.
// Copyright (c) 2026 Einheit Networks

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "einheit/cli/render/theme.h"

namespace einheit::cli::render {
namespace {

class TmpDir {
 public:
  TmpDir() {
    path_ =
        std::filesystem::temp_directory_path() /
        std::format("einheit_theme_{}_{}",
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

TEST(Theme, DefaultsDiffer) {
  const auto dark = DefaultDarkTheme();
  const auto light = DefaultLightTheme();
  EXPECT_NE(dark.dim, light.dim);
  EXPECT_NE(dark.emphasis, light.emphasis);
}

TEST(Theme, LoadsOverridesFromYaml) {
  TmpDir d;
  {
    std::ofstream f(d.Path() + "/theme.yaml");
    f << "good: Blue\nwarn: Magenta\n";
  }
  auto t = LoadTheme(d.Path() + "/theme.yaml");
  ASSERT_TRUE(t.has_value()) << t.error().message;
  EXPECT_EQ(t->good, "Blue");
  EXPECT_EQ(t->warn, "Magenta");
  // Unset keys keep defaults.
  EXPECT_EQ(t->bad, DefaultDarkTheme().bad);
}

TEST(Theme, MissingFileReportsError) {
  auto t = LoadTheme("/tmp/definitely-not-a-theme.yaml");
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error().code, ThemeError::NotFound);
}

TEST(Theme, DetectsLightFromColorFgBg) {
  // Set COLORFGBG to "0;15" (black on white) → should detect light.
  ::setenv("COLORFGBG", "0;15", 1);
  EXPECT_TRUE(DetectLightTerminal());
  // "15;0" (white on black) → dark.
  ::setenv("COLORFGBG", "15;0", 1);
  EXPECT_FALSE(DetectLightTerminal());
  ::unsetenv("COLORFGBG");
  EXPECT_FALSE(DetectLightTerminal());
}

}  // namespace einheit::cli::render
