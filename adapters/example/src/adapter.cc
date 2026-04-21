/// @file adapter.cc
/// @brief Example adapter implementation.
// Copyright (c) 2026 Einheit Networks

#include "adapters/example/adapter.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/table.h"
#include "einheit/cli/schema.h"

namespace einheit::adapters::example {
namespace {

using einheit::cli::CommandSpec;
using einheit::cli::ProductAdapter;
using einheit::cli::ProductMetadata;
using einheit::cli::protocol::Event;
using einheit::cli::protocol::Response;
using einheit::cli::render::Renderer;
using einheit::cli::schema::Schema;

// Baked-in schema — mirrors adapters/example/schema.yaml. Compiled
// into the binary so the adapter works without a separately-shipped
// file. Keep in sync with the on-disk version.
constexpr const char *kSchemaYaml = R"(
version: 1
product: example

config:
  hostname:
    type: string
    required: true
    help: "Appliance hostname"
    example: "einheit-demo"

  port:
    type: integer
    range: [1, 65535]
    default: "443"
    help: "Management API listen port"

  mode:
    type: enum
    values: [active, standby, off]
    default: "active"
    help: "Operating mode"

  admin_email:
    type: string
    help: "Notification address for alerts"

  management_network:
    type: cidr
    help: "CIDR that may reach the management port"
    example: "10.0.0.0/24"

  interfaces:
    type: map
    key: string
    value:
      type: object
      fields:
        address:
          type: cidr
          help: "Interface address + prefix"
        vlan:
          type: integer
          range: [1, 4094]
          help: "802.1Q VLAN tag"
        enabled:
          type: boolean
          default: "true"
          help: "Whether the interface is up"

types: {}
)";

auto LoadBakedSchema() -> std::shared_ptr<Schema> {
  // LoadSchema takes a path, so stage the baked YAML to tmp. Real
  // adapters that already ship a schema file can read it directly.
  const auto path =
      std::filesystem::temp_directory_path() /
      "einheit_example_schema.yaml";
  {
    std::ofstream f(path);
    f << kSchemaYaml;
  }
  auto s = einheit::cli::schema::LoadSchema(path.string());
  if (!s) return std::make_shared<Schema>();
  return *s;
}

class ExampleAdapter : public ProductAdapter {
 public:
  ExampleAdapter() : schema_(LoadBakedSchema()) {}

  auto Metadata() const -> ProductMetadata override {
    ProductMetadata m;
    m.id = "example";
    m.display_name = "Einheit Example Product";
    m.version = "0.0.1";
    m.banner = "einheit (example adapter)";
    m.prompt = "einheit";
    return m;
  }

  auto GetSchema() const -> const Schema & override {
    return *schema_;
  }

  auto ControlSocketPath() const -> std::string override {
    return "ipc:///var/run/einheit/example.ctl";
  }

  auto EventSocketPath() const -> std::string override {
    return "ipc:///var/run/einheit/example.pub";
  }

  auto Commands() const -> std::vector<CommandSpec> override {
    std::vector<CommandSpec> out;
    {
      CommandSpec c;
      c.path = "show status";
      c.wire_command = "show_status";
      c.help = "Show daemon status";
      out.push_back(std::move(c));
    }
    return out;
  }

  auto RenderResponse(const CommandSpec &cmd,
                      const Response &response,
                      Renderer &renderer) const -> void override {
    using einheit::cli::render::AddColumn;
    using einheit::cli::render::AddRow;
    using einheit::cli::render::Align;
    using einheit::cli::render::Cell;
    using einheit::cli::render::Priority;
    using einheit::cli::render::RenderFormatted;
    using einheit::cli::render::Semantic;

    if (response.error) {
      einheit::cli::render::Table t;
      AddColumn(t, "error", Align::Left, Priority::High);
      AddColumn(t, "message", Align::Left, Priority::High);
      AddRow(t, {
          Cell{response.error->code, Semantic::Bad},
          Cell{response.error->message, Semantic::Bad},
      });
      RenderFormatted(t, renderer);
      return;
    }

    (void)cmd;

    if (response.data.empty()) {
      einheit::cli::render::Table t;
      AddColumn(t, "status", Align::Left, Priority::High);
      AddRow(t, {Cell{"ok", Semantic::Good}});
      RenderFormatted(t, renderer);
      return;
    }

    // Parse the learning daemon's key=value lines into a two-column
    // table. `value` inherits a semantic from the key name so
    // commit_id → good, etc.
    einheit::cli::render::Table t;
    AddColumn(t, "field", Align::Left, Priority::High);
    AddColumn(t, "value", Align::Left, Priority::High);

    const std::string body(response.data.begin(),
                           response.data.end());
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty()) continue;
      const auto eq = line.find('=');
      Semantic val_semantic = Semantic::Default;
      std::string key, val;
      if (eq == std::string::npos) {
        key = line;
      } else {
        key = line.substr(0, eq);
        val = line.substr(eq + 1);
      }
      if (key == "commit_id" || key == "status") {
        val_semantic = Semantic::Good;
      } else if (key == "session") {
        val_semantic = Semantic::Info;
      } else if (val.empty() || val == "<none>") {
        val_semantic = Semantic::Dim;
      }
      AddRow(t, {
          Cell{key, Semantic::Emphasis},
          Cell{val, val_semantic},
      });
    }
    RenderFormatted(t, renderer);
  }

  auto EventTopicsFor(const CommandSpec &cmd) const
      -> std::vector<std::string> override {
    (void)cmd;
    return {};
  }

  auto RenderEvent(const std::string &topic, const Event &event,
                   Renderer &renderer) const -> void override {
    (void)topic;
    (void)event;
    (void)renderer;
  }

 private:
  std::shared_ptr<Schema> schema_;
};

}  // namespace

auto NewExampleAdapter()
    -> std::unique_ptr<einheit::cli::ProductAdapter> {
  return std::make_unique<ExampleAdapter>();
}

}  // namespace einheit::adapters::example
