/// @file test_watch.cc
/// @brief Watch subscription flow against the fake daemon.
// Copyright (c) 2026 Einheit Networks

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/table.h"
#include "einheit/cli/render/terminal_caps.h"
#include "einheit/cli/transport/zmq_local.h"
#include "einheit/cli/watch.h"

#include "tests/fake_daemon.h"

namespace einheit::cli::watch {
namespace {

using einheit::cli::testing::FakeDaemon;

class TopicsAdapter : public ProductAdapter {
 public:
  explicit TopicsAdapter(std::vector<std::string> topics)
      : topics_(std::move(topics)) {}

  auto Metadata() const -> ProductMetadata override { return {}; }
  auto GetSchema() const -> const schema::Schema & override {
    return schema_;
  }
  auto ControlSocketPath() const -> std::string override { return ""; }
  auto EventSocketPath() const -> std::string override { return ""; }
  auto Commands() const -> std::vector<CommandSpec> override {
    return {};
  }
  auto RenderResponse(const CommandSpec &, const protocol::Response &,
                      render::Renderer &) const -> void override {}
  auto EventTopicsFor(const CommandSpec &) const
      -> std::vector<std::string> override {
    return topics_;
  }

  // Record every event seen so tests can assert on topic + count.
  auto RenderEvent(const std::string &topic,
                   const protocol::Event &,
                   render::Renderer &renderer) const
      -> void override {
    std::lock_guard<std::mutex> lk(mu_);
    seen_.push_back(topic);
    renderer.Out() << topic << '\n';
  }

  auto Seen() const -> std::vector<std::string> {
    std::lock_guard<std::mutex> lk(mu_);
    return seen_;
  }

 private:
  std::vector<std::string> topics_;
  schema::Schema schema_;
  mutable std::mutex mu_;
  mutable std::vector<std::string> seen_;
};

}  // namespace

TEST(Watch, SubscribesAndRendersEvents) {
  FakeDaemon daemon([](const protocol::Request &) {
    return protocol::Response{};
  });

  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();
  auto tx = transport::NewZmqLocalTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  auto adapter =
      std::make_unique<TopicsAdapter>(std::vector<std::string>{
          "state.tunnels.", "state.routes."});

  CommandSpec watch_spec;
  watch_spec.path = "show tunnels";
  watch_spec.wire_command = "show_tunnels";

  std::ostringstream oss;
  render::Renderer renderer(oss, render::TerminalCaps{});

  WatchContext ctx;
  ctx.tx = tx->get();
  ctx.adapter = adapter.get();
  ctx.spec = &watch_spec;
  ctx.renderer = &renderer;

  std::atomic<bool> stop{false};

  // Run watch on a helper thread; publish events; then stop.
  std::thread publisher([&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    for (int i = 0; i < 3; ++i) {
      protocol::Event ev;
      ev.topic = "state.tunnels.munich";
      daemon.Publish(ev.topic, ev);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
  });

  auto result =
      RunWatch(ctx, stop, std::chrono::milliseconds(3000));
  publisher.join();

  ASSERT_TRUE(result.has_value()) << result.error().message;
  EXPECT_GE(*result, 1u);
  const auto seen = adapter->Seen();
  EXPECT_FALSE(seen.empty());
  EXPECT_EQ(seen.front(), "state.tunnels.munich");
}

TEST(Watch, RejectsCommandWithoutTopics) {
  FakeDaemon daemon([](const protocol::Request &) {
    return protocol::Response{};
  });

  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();
  auto tx = transport::NewZmqLocalTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  auto adapter = std::make_unique<TopicsAdapter>(
      std::vector<std::string>{});
  CommandSpec spec;
  spec.path = "show config";
  std::ostringstream oss;
  render::Renderer renderer(oss, render::TerminalCaps{});

  WatchContext ctx;
  ctx.tx = tx->get();
  ctx.adapter = adapter.get();
  ctx.spec = &spec;
  ctx.renderer = &renderer;

  std::atomic<bool> stop{true};
  auto r = RunWatch(ctx, stop, std::chrono::milliseconds(100));
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, WatchError::NoTopicsDeclared);
}

TEST(Watch, StopsWhenFlagSet) {
  FakeDaemon daemon([](const protocol::Request &) {
    return protocol::Response{};
  });

  transport::ZmqLocalConfig cfg;
  cfg.control_endpoint = daemon.ControlEndpoint();
  cfg.event_endpoint = daemon.EventEndpoint();
  auto tx = transport::NewZmqLocalTransport(cfg);
  ASSERT_TRUE(tx.has_value());
  ASSERT_TRUE((*tx)->Connect().has_value());

  auto adapter = std::make_unique<TopicsAdapter>(
      std::vector<std::string>{"state."});
  CommandSpec spec;
  spec.path = "show tunnels";
  std::ostringstream oss;
  render::Renderer renderer(oss, render::TerminalCaps{});

  WatchContext ctx;
  ctx.tx = tx->get();
  ctx.adapter = adapter.get();
  ctx.spec = &spec;
  ctx.renderer = &renderer;

  std::atomic<bool> stop{true};
  const auto started = std::chrono::steady_clock::now();
  auto r = RunWatch(ctx, stop, std::chrono::milliseconds(5000));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_TRUE(r.has_value());
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
}

}  // namespace einheit::cli::watch
