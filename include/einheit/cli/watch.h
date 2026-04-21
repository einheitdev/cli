/// @file watch.h
/// @brief `watch <cmd>` support — subscribe to adapter-declared
/// topics, re-render each arriving event via the adapter's
/// RenderEvent callback until the caller stops.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_WATCH_H_
#define INCLUDE_EINHEIT_CLI_WATCH_H_

#include <atomic>
#include <chrono>
#include <expected>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/error.h"
#include "einheit/cli/render/table.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::cli::watch {

/// Errors surfaced by the watch loop.
enum class WatchError {
  /// Adapter declared no topics for this command.
  NoTopicsDeclared,
  /// Transport refused a subscription.
  SubscribeFailed,
  /// Transport failed to unsubscribe cleanly.
  UnsubscribeFailed,
};

/// Everything a Watch loop needs. Kept POD so tests can construct
/// one without owning the Shell.
struct WatchContext {
  /// Connected transport.
  transport::Transport *tx = nullptr;
  /// Adapter providing topic names + RenderEvent.
  const ProductAdapter *adapter = nullptr;
  /// The command being watched (drives EventTopicsFor and
  /// RenderEvent dispatch).
  const CommandSpec *spec = nullptr;
  /// Renderer events are written to.
  render::Renderer *renderer = nullptr;
};

/// Subscribe to all topics the adapter declares for `ctx.spec` and
/// dispatch each arriving event to RenderEvent. Runs until `stop`
/// goes true or `max_duration` elapses.
/// @param ctx Watch context.
/// @param stop Stop flag polled between events. Caller signals exit
/// by setting it true.
/// @param max_duration Upper bound on how long to wait. Zero means
/// "no limit" (common for interactive `watch`).
/// @returns Number of events dispatched, or WatchError.
auto RunWatch(const WatchContext &ctx, std::atomic<bool> &stop,
              std::chrono::milliseconds max_duration)
    -> std::expected<std::size_t, Error<WatchError>>;

}  // namespace einheit::cli::watch

#endif  // INCLUDE_EINHEIT_CLI_WATCH_H_
