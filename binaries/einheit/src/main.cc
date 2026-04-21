/// @file main.cc
/// @brief Top-level `einheit` entry point. Parses argv, picks a
/// transport (local IPC or remote CurveZMQ based on --target), wires
/// up the example adapter, and hands off to RunShell or RunOneshot.
// Copyright (c) 2026 Einheit Networks

#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "adapters/example/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/curve_keys.h"
#include "einheit/cli/globals.h"
#include "einheit/cli/learning_daemon.h"
#include "einheit/cli/render/table.h"
#include "einheit/cli/render/terminal_caps.h"
#include "einheit/cli/schema.h"
#include "einheit/cli/shell.h"
#include "einheit/cli/target_config.h"
#include "einheit/cli/transport/zmq_local.h"
#include "einheit/cli/transport/zmq_remote.h"
#include "einheit/cli/workstation_state.h"

namespace {

auto BuildTreeWithAdapter(einheit::cli::CommandTree &tree,
                          einheit::cli::ProductAdapter &adapter)
    -> void {
  (void)einheit::cli::RegisterGlobals(tree);
  for (auto &spec : adapter.Commands()) {
    (void)einheit::cli::Register(tree, std::move(spec));
  }
}

auto ParseFormat(const std::string &s)
    -> einheit::cli::render::OutputFormat {
  using einheit::cli::render::OutputFormat;
  if (s == "json") return OutputFormat::Json;
  if (s == "yaml") return OutputFormat::Yaml;
  if (s == "set") return OutputFormat::Set;
  return OutputFormat::Table;
}

// Build a transport based on --target. Empty target => local IPC
// through the adapter's socket paths; non-empty => resolve against
// ~/.einheit/config and open a CurveZMQ tcp:// transport.
auto MakeTransport(const einheit::cli::ProductAdapter &adapter,
                   const std::string &target_name)
    -> std::unique_ptr<einheit::cli::transport::Transport> {
  using namespace einheit::cli;
  if (target_name.empty()) {
    transport::ZmqLocalConfig cfg;
    cfg.control_endpoint = adapter.ControlSocketPath();
    cfg.event_endpoint = adapter.EventSocketPath();
    auto tx = transport::NewZmqLocalTransport(cfg);
    if (!tx) {
      std::cerr << std::format("transport setup: {}\n",
                               tx.error().message);
      return nullptr;
    }
    if (auto r = (*tx)->Connect(); !r) {
      std::cerr << std::format("connect: {}\n", r.error().message);
      return nullptr;
    }
    return std::move(*tx);
  }

  auto cfg = target::LoadFromHome();
  if (!cfg) {
    std::cerr << std::format("target config: {}\n",
                             cfg.error().message);
    return nullptr;
  }
  auto entry = target::Resolve(*cfg, target_name);
  if (!entry) {
    std::cerr << std::format("target: {}\n", entry.error().message);
    return nullptr;
  }

  auto keys = curve::ReadFromDisk(
      std::filesystem::path((*entry)->client_secret_key_path)
          .parent_path()
          .string(),
      std::filesystem::path((*entry)->client_secret_key_path)
          .stem()
          .string());
  if (!keys) {
    std::cerr << std::format("client keys: {}\n", keys.error().message);
    return nullptr;
  }

  transport::ZmqRemoteConfig rcfg;
  rcfg.control_endpoint = (*entry)->control_endpoint;
  rcfg.event_endpoint = (*entry)->event_endpoint;
  rcfg.server_public_key = (*entry)->server_public_key;
  rcfg.client_public_key = keys->public_key;
  rcfg.client_secret_key = keys->secret_key;

  auto tx = transport::NewZmqRemoteTransport(rcfg);
  if (!tx) {
    std::cerr << std::format("transport setup: {}\n",
                             tx.error().message);
    return nullptr;
  }
  if (auto r = (*tx)->Connect(); !r) {
    std::cerr << std::format("connect: {}\n", r.error().message);
    return nullptr;
  }
  return std::move(*tx);
}

}  // namespace

auto HandleKeyGenerate(const std::string &name) -> int {
  using namespace einheit::cli;
  const char *home = std::getenv("HOME");
  if (!home) {
    std::cerr << "HOME unset\n";
    return 1;
  }
  const auto base = std::format("{}/.einheit/keys", home);
  auto pair = curve::Generate();
  if (!pair) {
    std::cerr << std::format("generate: {}\n", pair.error().message);
    return 1;
  }
  if (auto r = curve::WriteToDisk(base, name, *pair); !r) {
    std::cerr << std::format("write: {}\n", r.error().message);
    return 1;
  }
  std::cout << std::format(
      "Generated:\n  {}/{}.secret  (mode 0600)\n  {}/{}.public\n",
      base, name, base, name);
  std::cout << std::format("public key: {}\n", pair->public_key);
  return 0;
}

auto HandleUse(const std::string &target_name) -> int {
  using namespace einheit::cli;
  // Verify the target exists in config before switching.
  auto cfg = target::LoadFromHome();
  if (!cfg) {
    std::cerr << std::format("target config: {}\n",
                             cfg.error().message);
    return 1;
  }
  auto entry = target::Resolve(*cfg, target_name);
  if (!entry) {
    std::cerr << std::format("use: {}\n", entry.error().message);
    return 1;
  }

  workstation::State state;
  if (auto prior = workstation::Load(workstation::DefaultPath()); prior) {
    state = *prior;
  }
  state.active_target = target_name;
  if (auto r = workstation::Save(workstation::DefaultPath(), state);
      !r) {
    std::cerr << std::format("save state: {}\n", r.error().message);
    return 1;
  }
  std::cout << std::format("using target '{}'\n", target_name);
  return 0;
}

auto main(int argc, char **argv) -> int {
  CLI::App app{"einheit — Einheit Networks CLI"};
  app.option_defaults()->ignore_case();
  app.allow_extras();

  std::string color = "auto";
  bool ascii = false;
  int width = 0;
  std::string format = "table";
  std::string target;
  bool learn = false;
  bool trace = false;
  app.add_option("--color", color, "always|never|auto");
  app.add_flag("--ascii", ascii, "Force ASCII borders");
  app.add_option("--width", width, "Override detected width");
  app.add_option("--format", format, "table|json|yaml|set");
  app.add_option("--target", target,
                 "Named target from ~/.einheit/config");
  app.add_flag("--learn", learn,
               "Run against an in-process stub daemon "
               "(no real appliance needed)");
  app.add_flag("--trace", trace,
               "Print wire traffic on stderr (best with --learn)");

  // Client-side subcommands that don't need a transport.
  auto *key_cmd = app.add_subcommand(
      "key", "Manage Curve client keypairs");
  auto *key_gen = key_cmd->add_subcommand(
      "generate", "Generate a new keypair");
  std::string key_name;
  key_gen->add_option("name", key_name, "Keypair filename stem")
      ->required();

  auto *use_cmd = app.add_subcommand(
      "use", "Set the default target for subsequent invocations");
  std::string use_name;
  use_cmd->add_option("target", use_name, "Target name from config")
      ->required();

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  if (*key_gen) return HandleKeyGenerate(key_name);
  if (*use_cmd) return HandleUse(use_name);

  using namespace einheit::cli;

  // If no explicit --target, fall back to the persisted state.
  if (target.empty()) {
    if (auto saved = workstation::Load(workstation::DefaultPath());
        saved && saved->active_target) {
      target = *saved->active_target;
    }
  }

  render::CapOverrides ov;
  ov.color = (color == "always" ? 1 : color == "never" ? 0 : -1);
  ov.force_ascii = ascii;
  ov.width = static_cast<std::uint16_t>(width);
  const auto caps = render::ApplyOverrides(render::DetectTerminal(), ov);

  auto adapter = einheit::adapters::example::NewExampleAdapter();

  // Learning mode: spawn an in-process daemon and point the transport
  // at its tmpdir ipc:// socket. The daemon validates set/delete
  // against the adapter's schema so operators see proper validation
  // errors instead of a flat-map fallback. Daemon is kept alive for
  // the whole session via RAII.
  std::unique_ptr<learning::LearningDaemon> learn_daemon;
  std::string learn_ctl;
  std::string learn_pub;
  if (learn) {
    // Non-owning shared_ptr — the adapter keeps the schema alive.
    std::shared_ptr<const schema::Schema> schema(
        &adapter->GetSchema(), [](const schema::Schema *) {});
    learn_daemon = std::make_unique<learning::LearningDaemon>(
        trace ? &std::cerr : nullptr, schema);
    learn_ctl = learn_daemon->ControlEndpoint();
    learn_pub = learn_daemon->EventEndpoint();
    if (trace) std::cerr << "Tracing wire traffic on stderr.\n";
  }

  std::unique_ptr<transport::Transport> tx;
  if (learn) {
    transport::ZmqLocalConfig cfg;
    cfg.control_endpoint = learn_ctl;
    cfg.event_endpoint = learn_pub;
    auto built = transport::NewZmqLocalTransport(cfg);
    if (!built || !(*built)->Connect()) {
      std::cerr << "learning transport setup failed\n";
      return 1;
    }
    tx = std::move(*built);
  } else {
    tx = MakeTransport(*adapter, target);
    if (!tx) return 1;
  }

  shell::Shell s;
  s.tx = std::move(tx);
  s.caps = caps;
  s.learning_mode = learn;
  s.target_name = target;
  BuildTreeWithAdapter(s.tree, *adapter);
  s.adapter = std::move(adapter);

  // In learning mode there is no real auth — pretend the operator is
  // admin so configure/set/commit all work and the user gets to see
  // the whole lifecycle.
  if (learn) {
    s.caller.user = "learner";
    s.caller.role = RoleGate::AdminOnly;
  }

  const auto leftovers = app.remaining();
  if (!leftovers.empty()) {
    auto r = shell::RunOneshot(s, leftovers);
    if (!r) {
      std::cerr << std::format("oneshot: {}\n", r.error().message);
      return 1;
    }
    if (r->response &&
        r->response->status == protocol::ResponseStatus::Error) {
      return 1;
    }
    return 0;
  }

  auto rc = RunShell(s);
  if (!rc) {
    std::cerr << std::format("shell: {}\n", rc.error().message);
    return 1;
  }
  (void)format;
  return 0;
}
