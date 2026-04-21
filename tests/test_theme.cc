/// @file test_theme.cc
/// @brief Theme palette + YAML loading tests.
// Copyright (c) 2026 Einheit Networks

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "einheit/cli/render/terminal_caps.h"
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

TEST(Theme, PaletteFactoriesReturnValidThemes) {
  // Smoke: both factories return themes without throwing.
  (void)DefaultDarkTrueColor();
  (void)DefaultDarkAnsi();
  SUCCEED();
}

TEST(Theme, PickThemeByCaps) {
  TerminalCaps caps_true;
  caps_true.colors = ColorDepth::TrueColor;
  caps_true.unicode = true;
  caps_true.is_tty = true;
  (void)PickTheme(caps_true);

  TerminalCaps caps_ansi;
  caps_ansi.colors = ColorDepth::Ansi16;
  (void)PickTheme(caps_ansi);

  TerminalCaps caps_none;
  caps_none.colors = ColorDepth::None;
  caps_none.force_plain = true;
  (void)PickTheme(caps_none);
  SUCCEED();
}

TEST(Theme, ParseHexAndName) {
  const auto fallback = ftxui::Color::Red;
  // Hex literal.
  (void)ParseColor("#9ECE6A", fallback);
  // Enum name, multiple spellings.
  (void)ParseColor("GreenLight", fallback);
  (void)ParseColor("green-light", fallback);
  (void)ParseColor("green_light", fallback);
  // Unknown → fallback (not crashable).
  (void)ParseColor("not-a-color", fallback);
  SUCCEED();
}

TEST(Theme, LoadsFromYamlWithHexAndNames) {
  TmpDir d;
  {
    std::ofstream f(d.Path() + "/theme.yaml");
    f << "good: \"#9ECE6A\"\n"
         "warn: \"#E0AF68\"\n"
         "bad: \"RedLight\"\n";
  }
  auto t = LoadTheme(d.Path() + "/theme.yaml", DefaultDarkAnsi());
  ASSERT_TRUE(t.has_value()) << t.error().message;
  SUCCEED();
}

TEST(Theme, MissingFileReportsNotFound) {
  auto t = LoadTheme("/tmp/definitely-not-a-theme.yaml",
                     DefaultDarkAnsi());
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error().code, ThemeError::NotFound);
}

}  // namespace einheit::cli::render
