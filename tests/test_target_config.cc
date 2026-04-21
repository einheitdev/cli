/// @file test_target_config.cc
/// @brief Target config parser + resolver tests.
// Copyright (c) 2026 Einheit Networks

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "einheit/cli/target_config.h"

namespace einheit::cli::target {
namespace {

class ScopedYaml {
 public:
  explicit ScopedYaml(const std::string &body) {
    path_ = std::filesystem::temp_directory_path() /
            ("einheit_target_" + std::to_string(::getpid()) +
             "_" + std::to_string(counter_++) + ".yaml");
    std::ofstream(path_) << body;
  }
  ~ScopedYaml() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  auto Path() const -> std::string { return path_.string(); }

 private:
  static inline int counter_ = 0;
  std::filesystem::path path_;
};

constexpr const char *kSample = R"(
targets:
  - name: berlin
    endpoint: tcp://gateway-berlin.lan:7541
    event_endpoint: tcp://gateway-berlin.lan:7542
    server_key: "abc123"
    client_key: /home/karl/.einheit/keys/karl.secret
  - name: munich
    endpoint: tcp://gateway-munich.lan:7541
    server_key: "def456"
    client_key: /home/karl/.einheit/keys/karl.secret

default: berlin
)";

}  // namespace

TEST(TargetConfig, LoadsTargets) {
  ScopedYaml f(kSample);
  auto cfg = LoadFromFile(f.Path());
  ASSERT_TRUE(cfg.has_value()) << cfg.error().message;
  ASSERT_EQ(cfg->targets.size(), 2u);
  EXPECT_EQ(cfg->targets[0].name, "berlin");
  EXPECT_EQ(cfg->targets[0].control_endpoint,
            "tcp://gateway-berlin.lan:7541");
  EXPECT_EQ(cfg->targets[0].event_endpoint,
            "tcp://gateway-berlin.lan:7542");
  EXPECT_EQ(cfg->targets[0].server_public_key, "abc123");
  EXPECT_EQ(cfg->default_target.value_or(""), "berlin");
}

TEST(TargetConfig, ResolvesExplicitTarget) {
  ScopedYaml f(kSample);
  auto cfg = LoadFromFile(f.Path());
  ASSERT_TRUE(cfg.has_value());
  auto t = Resolve(*cfg, "munich");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ((*t)->name, "munich");
}

TEST(TargetConfig, ResolvesDefaultWhenNameEmpty) {
  ScopedYaml f(kSample);
  auto cfg = LoadFromFile(f.Path());
  ASSERT_TRUE(cfg.has_value());
  auto t = Resolve(*cfg, "");
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ((*t)->name, "berlin");
}

TEST(TargetConfig, UnknownTargetRejected) {
  ScopedYaml f(kSample);
  auto cfg = LoadFromFile(f.Path());
  ASSERT_TRUE(cfg.has_value());
  auto t = Resolve(*cfg, "atlantis");
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error().code, TargetError::UnknownTarget);
}

TEST(TargetConfig, NoDefaultWhenAbsent) {
  ScopedYaml f(R"(
targets:
  - name: only
    endpoint: tcp://1.1.1.1:1000
)");
  auto cfg = LoadFromFile(f.Path());
  ASSERT_TRUE(cfg.has_value());
  EXPECT_FALSE(cfg->default_target.has_value());
  auto t = Resolve(*cfg, "");
  ASSERT_FALSE(t.has_value());
  EXPECT_EQ(t.error().code, TargetError::NoDefault);
}

TEST(TargetConfig, MissingFileReportsNotFound) {
  auto cfg = LoadFromFile("/tmp/this-does-not-exist-einheit.yaml");
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().code, TargetError::NotFound);
}

TEST(TargetConfig, InvalidEntryRejected) {
  ScopedYaml f(R"(
targets:
  - endpoint: tcp://1.1.1.1:1000
)");
  auto cfg = LoadFromFile(f.Path());
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().code, TargetError::InvalidTarget);
}

}  // namespace einheit::cli::target
