/// @file adapter.h
/// @brief ProductAdapter — the contract every product plugs into.
///
/// An adapter is a thin, declarative object: metadata, schema,
/// commands, renderers. No business logic; that lives in the
/// product's daemon. The framework owns the shell, transport,
/// dispatch, and rendering primitives.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_ADAPTER_H_
#define INCLUDE_EINHEIT_CLI_ADAPTER_H_

#include <memory>
#include <string>
#include <vector>

#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/schema.h"

namespace einheit::cli {

namespace render { class Renderer; }

/// Static metadata declared by each adapter.
struct ProductMetadata {
  /// Short product identifier ("g-gateway", "hd-relay").
  std::string id;
  /// Display name shown in banners and help.
  std::string display_name;
  /// Adapter version string.
  std::string version;
  /// Banner text shown on shell entry.
  std::string banner;
  /// Prompt string fragment used in command mode.
  std::string prompt;
};

/// CLI-side contract between framework and product. See the framework
/// spec for the daemon-side counterpart.
class ProductAdapter {
 public:
  virtual ~ProductAdapter() = default;

  /// Static product metadata.
  virtual auto Metadata() const -> ProductMetadata = 0;

  /// The loaded config schema.
  virtual auto GetSchema() const -> const schema::Schema & = 0;

  /// ZMQ control endpoint for the daemon.
  /// Typically "ipc:///var/run/einheit/<product>.ctl".
  virtual auto ControlSocketPath() const -> std::string = 0;

  /// ZMQ event endpoint for the daemon.
  /// Typically "ipc:///var/run/einheit/<product>.pub".
  virtual auto EventSocketPath() const -> std::string = 0;

  /// Commands this adapter contributes to the tree.
  virtual auto Commands() const -> std::vector<CommandSpec> = 0;

  /// Render a Response for a command. Adapter decodes the MessagePack
  /// `data` blob and writes output via the renderer.
  virtual auto RenderResponse(const CommandSpec &cmd,
                              const protocol::Response &response,
                              render::Renderer &renderer) const
      -> void = 0;

  /// Event topics a command (like `watch show tunnels`) needs.
  virtual auto EventTopicsFor(const CommandSpec &cmd) const
      -> std::vector<std::string> = 0;

  /// Incremental render for an event delivered on a subscribed topic.
  virtual auto RenderEvent(const std::string &topic,
                           const protocol::Event &event,
                           render::Renderer &renderer) const
      -> void = 0;
};

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_ADAPTER_H_
