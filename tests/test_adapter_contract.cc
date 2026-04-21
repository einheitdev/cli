/// @file test_adapter_contract.cc
/// @brief Exercises ValidateAdapter against well-formed and
/// deliberately-broken adapters.
// Copyright (c) 2026 Einheit Networks

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/adapter.h"
#include "einheit/cli/adapter_contract.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/table.h"

#include "adapters/example/adapter.h"

namespace einheit::cli::contract {
namespace {

class GoodAdapter : public ProductAdapter {
 public:
  GoodAdapter() = default;
  auto Metadata() const -> ProductMetadata override { return {}; }
  auto GetSchema() const -> const schema::Schema & override {
    return schema_;
  }
  auto ControlSocketPath() const -> std::string override { return ""; }
  auto EventSocketPath() const -> std::string override { return ""; }
  auto Commands() const -> std::vector<CommandSpec> override {
    CommandSpec a;
    a.path = "show tunnels";
    a.wire_command = "show_tunnels";
    CommandSpec b;
    b.path = "watch tunnels";
    b.wire_command = "watch_tunnels";
    return {a, b};
  }
  auto RenderResponse(const CommandSpec &, const protocol::Response &,
                      render::Renderer &) const -> void override {}
  auto EventTopicsFor(const CommandSpec &spec) const
      -> std::vector<std::string> override {
    if (spec.path == "watch tunnels") return {"state.tunnels."};
    return {};
  }
  auto RenderEvent(const std::string &, const protocol::Event &,
                   render::Renderer &) const -> void override {}

 private:
  schema::Schema schema_;
};

class ThrowingRenderAdapter : public GoodAdapter {
 public:
  auto Commands() const -> std::vector<CommandSpec> override {
    CommandSpec s;
    s.path = "show broken";
    s.wire_command = "show_broken";
    return {s};
  }
  auto RenderResponse(const CommandSpec &, const protocol::Response &,
                      render::Renderer &) const -> void override {
    throw std::runtime_error("render failure");
  }
};

class DuplicateWireAdapter : public GoodAdapter {
 public:
  auto Commands() const -> std::vector<CommandSpec> override {
    CommandSpec a;
    a.path = "show a";
    a.wire_command = "same";
    CommandSpec b;
    b.path = "show b";
    b.wire_command = "same";
    return {a, b};
  }
};

class InvalidSpecAdapter : public GoodAdapter {
 public:
  auto Commands() const -> std::vector<CommandSpec> override {
    CommandSpec bad;
    bad.path = "";
    bad.wire_command = "";
    return {bad};
  }
};

class ThrowingEventAdapter : public GoodAdapter {
 public:
  auto Commands() const -> std::vector<CommandSpec> override {
    CommandSpec s;
    s.path = "watch bad";
    s.wire_command = "watch_bad";
    return {s};
  }
  auto EventTopicsFor(const CommandSpec &) const
      -> std::vector<std::string> override {
    return {"state.bad."};
  }
  auto RenderEvent(const std::string &, const protocol::Event &,
                   render::Renderer &) const -> void override {
    throw std::runtime_error("event render failure");
  }
};

}  // namespace

TEST(AdapterContract, AcceptsWellFormedAdapter) {
  GoodAdapter a;
  auto r = ValidateAdapter(a);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->commands_checked, 2u);
  EXPECT_EQ(r->topics_checked, 1u);
}

TEST(AdapterContract, RejectsThrowingRenderResponse) {
  ThrowingRenderAdapter a;
  auto r = ValidateAdapter(a);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ContractError::RenderResponseThrew);
  EXPECT_NE(r.error().message.find("show broken"),
            std::string::npos);
}

TEST(AdapterContract, RejectsDuplicateWireCommand) {
  DuplicateWireAdapter a;
  auto r = ValidateAdapter(a);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ContractError::DuplicateWireCommand);
}

TEST(AdapterContract, RejectsInvalidSpec) {
  InvalidSpecAdapter a;
  auto r = ValidateAdapter(a);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ContractError::InvalidSpec);
}

TEST(AdapterContract, RejectsThrowingRenderEvent) {
  ThrowingEventAdapter a;
  auto r = ValidateAdapter(a);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ContractError::RenderEventThrew);
}

TEST(AdapterContract, ShippedExampleAdapterPasses) {
  auto adapter =
      einheit::adapters::example::NewExampleAdapter();
  auto r = ValidateAdapter(*adapter);
  EXPECT_TRUE(r.has_value()) << r.error().message;
}

}  // namespace einheit::cli::contract
