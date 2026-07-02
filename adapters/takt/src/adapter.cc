/// @file adapter.cc
/// @brief takt CLI adapter. Maps takt commands to
/// takt-service ZMQ requests and renders JSON responses
/// as formatted tables.
// Copyright (c) 2026 Einheit Networks

#include "adapters/takt/adapter.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/table.h"
#include "einheit/cli/schema.h"

#include "takt_transport.h"

namespace einheit::adapters::takt {
namespace {

using einheit::cli::ArgSpec;
using einheit::cli::CommandSpec;
using einheit::cli::ProductAdapter;
using einheit::cli::ProductMetadata;
using einheit::cli::protocol::Event;
using einheit::cli::protocol::Response;
using einheit::cli::render::Renderer;
using einheit::cli::schema::Schema;

constexpr const char *kSchemaYaml = R"(
version: 1
product: takt

config: {}
types: {}
)";

auto LoadBakedSchema() -> std::shared_ptr<Schema> {
  const auto path =
      std::filesystem::temp_directory_path() /
      "einheit_takt_schema.yaml";
  {
    std::ofstream f(path);
    f << kSchemaYaml;
  }
  auto s = einheit::cli::schema::LoadSchema(
      path.string());
  if (!s) return std::make_shared<Schema>();
  return *s;
}

/// Parse the JSON response data from takt-service.
auto ParseData(const Response &r) -> nlohmann::json {
  if (r.data.empty()) return nlohmann::json{};
  try {
    auto txt = std::string(r.data.begin(), r.data.end());
    auto j = nlohmann::json::parse(txt);
    if (j.contains("data")) return j["data"];
    return j;
  } catch (...) {
    return nlohmann::json{};
  }
}

/// Render a JSON array as a key-value table.
void RenderKvTable(const nlohmann::json &rows,
                   Renderer &renderer) {
  using einheit::cli::render::AddColumn;
  using einheit::cli::render::AddRow;
  using einheit::cli::render::Align;
  using einheit::cli::render::Cell;
  using einheit::cli::render::Priority;
  using einheit::cli::render::RenderFormatted;
  using einheit::cli::render::Semantic;
  einheit::cli::render::Table t;
  if (rows.empty() || !rows.is_array()) {
    renderer.Out() << "  (no data)\n";
    return;
  }
  auto first = rows[0];
  if (!first.is_object()) {
    renderer.Out() << "  (no data)\n";
    return;
  }
  std::vector<std::string> keys;
  for (auto it = first.begin(); it != first.end();
       ++it) {
    keys.push_back(it.key());
    AddColumn(t, it.key(), Align::Left, Priority::High);
  }
  for (const auto &row : rows) {
    std::vector<Cell> cells;
    for (const auto &k : keys) {
      auto val = row.value(k, nlohmann::json{});
      std::string text;
      if (val.is_string()) {
        text = val.get<std::string>();
      } else if (val.is_null()) {
        text = "—";
      } else {
        text = val.dump();
      }
      auto sem = Semantic::Default;
      if (k == "status" || k == "state") {
        if (text == "running" || text == "passed" ||
            text == "completed" || text == "clean") {
          sem = Semantic::Good;
        } else if (text == "failed" ||
                   text == "cancelled") {
          sem = Semantic::Bad;
        } else if (text == "queued" ||
                   text == "pending") {
          sem = Semantic::Warn;
        }
      }
      cells.push_back(Cell{text, sem});
    }
    AddRow(t, cells);
  }
  RenderFormatted(t, renderer);
}

/// Render a single JSON object as a vertical detail.
void RenderDetail(const nlohmann::json &obj,
                  Renderer &renderer) {
  using einheit::cli::render::AddColumn;
  using einheit::cli::render::AddRow;
  using einheit::cli::render::Align;
  using einheit::cli::render::Cell;
  using einheit::cli::render::Priority;
  using einheit::cli::render::RenderFormatted;
  using einheit::cli::render::Semantic;
  einheit::cli::render::Table t;
  AddColumn(t, "field", Align::Left, Priority::High);
  AddColumn(t, "value", Align::Left, Priority::High);
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    std::string val;
    if (it->is_string()) {
      val = it->get<std::string>();
    } else if (it->is_null()) {
      val = "—";
    } else {
      val = it->dump();
    }
    AddRow(t, {
        Cell{it.key(), Semantic::Emphasis},
        Cell{val, Semantic::Default},
    });
  }
  RenderFormatted(t, renderer);
}

class TaktAdapter : public ProductAdapter {
 public:
  TaktAdapter() : schema_(LoadBakedSchema()) {}

  auto Metadata() const -> ProductMetadata override {
    return {
        .id = "takt",
        .display_name = "takt pipeline orchestrator",
        .version = "0.0.1",
        .banner = "takt",
        .prompt = "takt",
    };
  }

  auto GetSchema() const -> const Schema & override {
    return *schema_;
  }

  auto ControlSocketPath() const
      -> std::string override {
    return "ipc:///home/karl/dev/takt/"
           ".state/takt-cmd.sock";
  }

  auto EventSocketPath() const
      -> std::string override {
    return "ipc:///home/karl/dev/takt/"
           ".state/takt-pub.sock";
  }

  auto Commands() const
      -> std::vector<CommandSpec> override {
    std::vector<CommandSpec> out;

    auto add = [&](std::string path,
                   std::string wire,
                   std::string help,
                   std::vector<ArgSpec> args = {}) {
      CommandSpec c;
      c.path = std::move(path);
      c.wire_command = std::move(wire);
      c.help = std::move(help);
      c.args = std::move(args);
      out.push_back(std::move(c));
    };

    add("show workspaces", "list_workspaces",
        "List all workspaces");
    add("show workspace", "get_workspace",
        "Show workspace details",
        {{"name", "Workspace name", true}});
    add("show targets", "list_targets",
        "List all targets");
    add("show runs", "list_runs",
        "Show pipeline run history",
        {{"workspace", "Filter by workspace", false}});
    add("show run", "get_run_detail",
        "Show run details",
        {{"id", "Run ID", true}});
    add("show agents", "list_agents",
        "Show active agents");
    add("show pipeline", "get_pipeline",
        "Show pipeline config for a workspace",
        {{"workspace", "Workspace name", true}});
    add("pipeline run", "trigger_run",
        "Trigger a pipeline run",
        {{"workspace", "Workspace name", true}});
    add("target claim", "claim_target",
        "Claim a target for a workspace",
        {{"name", "Target name", true},
         {"workspace", "Workspace name", true}});
    add("target release", "release_target",
        "Release a claimed target",
        {{"name", "Target name", true}});
    add("target up", "target_up",
        "Start a VM target",
        {{"name", "Target name", true}});
    add("target down", "target_down",
        "Stop a VM target",
        {{"name", "Target name", true}});
    add("workspace create", "create_workspace",
        "Create a new workspace",
        {{"name", "Workspace name", true},
         {"repos", "Repo names (space-separated)",
          true}});
    add("workspace delete", "delete_workspace",
        "Delete a workspace",
        {{"name", "Workspace name", true}});
    add("watch runs", "",
        "Watch live run events");
    add("watch agents", "",
        "Watch live agent state");

    return out;
  }

  auto RenderResponse(const CommandSpec &cmd,
                      const Response &response,
                      Renderer &renderer) const
      -> void override {
    if (response.error) {
      einheit::cli::render::RenderError(
          response.error->code,
          response.error->message,
          response.error->hint, renderer);
      return;
    }
    auto data = ParseData(response);
    if (data.is_null() || data.empty()) {
      renderer.Out() << "  ok\n";
      return;
    }
    if (data.is_array()) {
      RenderKvTable(data, renderer);
    } else if (data.is_object()) {
      if (data.contains("run") &&
          data.contains("steps")) {
        renderer.Out() << "  Run:\n";
        RenderDetail(data["run"], renderer);
        renderer.Out() << "\n  Steps:\n";
        RenderKvTable(data["steps"], renderer);
      } else {
        RenderDetail(data, renderer);
      }
    } else {
      renderer.Out() << "  " << data.dump(2) << "\n";
    }
  }

  auto EventTopicsFor(const CommandSpec &cmd) const
      -> std::vector<std::string> override {
    if (cmd.path == "watch runs") {
      return {"pipeline.event", "step.update"};
    }
    if (cmd.path == "watch agents") {
      return {"agent.update"};
    }
    return {};
  }

  auto RenderEvent(const std::string &topic,
                   const Event &event,
                   Renderer &renderer) const
      -> void override {
    try {
      auto text = std::string(
          event.data.begin(), event.data.end());
      auto j = nlohmann::json::parse(text);
      renderer.Out() << std::format(
          "  [{}] {}\n", topic, j.dump());
    } catch (...) {
      renderer.Out() << std::format(
          "  [{}] (unparseable)\n", topic);
    }
  }

 private:
  std::shared_ptr<Schema> schema_;
};

}  // namespace

auto NewTaktAdapter()
    -> std::unique_ptr<einheit::cli::ProductAdapter> {
  return std::make_unique<TaktAdapter>();
}

auto NewTaktTransport(
    const std::string &control,
    const std::string &event)
    -> std::expected<
        std::unique_ptr<cli::transport::Transport>,
        cli::Error<cli::transport::TransportError>> {
  TaktTransportConfig cfg;
  cfg.control_endpoint = control;
  cfg.event_endpoint = event;
  return MakeTaktTransport(std::move(cfg));
}

}  // namespace einheit::adapters::takt
