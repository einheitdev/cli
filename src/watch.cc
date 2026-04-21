/// @file watch.cc
/// @brief Subscribe-and-re-render loop for `watch <cmd>`.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/watch.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "einheit/cli/protocol/envelope.h"

namespace einheit::cli::watch {
namespace {

auto MakeError(WatchError code, std::string message)
    -> Error<WatchError> {
  return Error<WatchError>{code, std::move(message)};
}

}  // namespace

auto RunWatch(const WatchContext &ctx, std::atomic<bool> &stop,
              std::chrono::milliseconds max_duration)
    -> std::expected<std::size_t, Error<WatchError>> {
  const auto topics = ctx.adapter->EventTopicsFor(*ctx.spec);
  if (topics.empty()) {
    return std::unexpected(MakeError(
        WatchError::NoTopicsDeclared,
        "adapter declared no topics for: " + ctx.spec->path));
  }

  std::mutex count_mu;
  std::size_t count = 0;

  transport::EventCallback on_event =
      [&](const protocol::Event &ev) {
        std::lock_guard<std::mutex> lk(count_mu);
        ++count;
        ctx.adapter->RenderEvent(ev.topic, ev, *ctx.renderer);
      };

  std::vector<std::string> subscribed;
  subscribed.reserve(topics.size());
  for (const auto &t : topics) {
    auto r = ctx.tx->Subscribe(t, on_event);
    if (!r) {
      for (const auto &s : subscribed) (void)ctx.tx->Unsubscribe(s);
      return std::unexpected(MakeError(
          WatchError::SubscribeFailed, r.error().message));
    }
    subscribed.push_back(t);
  }

  const auto deadline =
      std::chrono::steady_clock::now() + max_duration;
  while (!stop.load()) {
    if (max_duration.count() > 0 &&
        std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  for (const auto &t : subscribed) {
    auto r = ctx.tx->Unsubscribe(t);
    if (!r) {
      return std::unexpected(MakeError(
          WatchError::UnsubscribeFailed, r.error().message));
    }
  }

  std::lock_guard<std::mutex> lk(count_mu);
  return count;
}

}  // namespace einheit::cli::watch
