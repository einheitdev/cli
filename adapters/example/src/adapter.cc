/// @file adapter.cc
/// @brief Example adapter implementation.
// Copyright (c) 2026 Einheit Networks

#include "adapters/example/adapter.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/render/sparkline.h"
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
    {
      // Framework-local — rendered from subscribed events only,
      // not a wire RPC. Used with `watch metrics` to demo the
      // sparkline column.
      CommandSpec c;
      c.path = "metrics";
      c.wire_command = "";
      c.help = "Show live metric series (use with `watch`)";
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
      // If the daemon packed a "did you mean" suggestion into the
      // message (common for schema validation errors), split it out
      // so the suggestion renders on the yellow hint line.
      std::string msg = response.error->message;
      std::string hint = response.error->hint;
      if (hint.empty()) {
        if (const auto pos = msg.find(" — ");
            pos != std::string::npos) {
          hint = msg.substr(pos + 5);
          msg = msg.substr(0, pos);
        }
      }
      einheit::cli::render::RenderError(response.error->code, msg,
                                        hint, renderer);
      return;
    }

    if (response.data.empty()) {
      const auto &path = cmd.path;
      if (path == "show config") {
        renderer.Out()
            << "  (no configuration yet — run `configure` then "
               "`set`)\n";
      } else if (path == "show commits") {
        renderer.Out()
            << "  (no commits yet — run `commit` to record one)\n";
      } else {
        einheit::cli::render::Table t;
        AddColumn(t, "status", Align::Left, Priority::High);
        AddRow(t, {Cell{"ok", Semantic::Good}});
        RenderFormatted(t, renderer);
      }
      return;
    }

    // Parse the learning daemon's key=value lines. `show_commit`
    // prefixes each line with a diff marker (+/-/~/=); colour
    // accordingly. Plain rows (no marker) are shown Emphasis/Default.
    einheit::cli::render::Table t;
    AddColumn(t, "field", Align::Left, Priority::High);
    AddColumn(t, "value", Align::Left, Priority::High);

    const std::string body(response.data.begin(),
                           response.data.end());
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
      if (line.empty()) continue;
      Semantic key_sem = Semantic::Emphasis;
      Semantic val_sem = Semantic::Default;
      char marker = 0;
      if (line[0] == '+' || line[0] == '-' || line[0] == '~' ||
          line[0] == '=') {
        marker = line[0];
        line.erase(0, 1);
        switch (marker) {
          case '+': key_sem = val_sem = Semantic::Good; break;
          case '-': key_sem = val_sem = Semantic::Bad; break;
          case '~': key_sem = val_sem = Semantic::Warn; break;
          case '=': key_sem = val_sem = Semantic::Dim; break;
        }
      }
      const auto eq = line.find('=');
      std::string key, val;
      if (eq == std::string::npos) {
        key = line;
      } else {
        key = line.substr(0, eq);
        val = line.substr(eq + 1);
      }
      if (marker == 0) {
        if (key == "commit_id" || key == "status") {
          val_sem = Semantic::Good;
        } else if (key == "session") {
          val_sem = Semantic::Info;
        } else if (val.empty() || val == "<none>") {
          val_sem = Semantic::Dim;
        }
      }
      std::string key_label = key;
      if (marker) key_label = std::format("{} {}", marker, key);
      AddRow(t, {Cell{key_label, key_sem}, Cell{val, val_sem}});
    }
    RenderFormatted(t, renderer);
  }

  auto EventTopicsFor(const CommandSpec &cmd) const
      -> std::vector<std::string> override {
    if (cmd.path == "metrics") return {"state.metrics."};
    return {};
  }

  auto RenderEvent(const std::string &topic, const Event &event,
                   Renderer &renderer) const -> void override {
    using einheit::cli::render::AddColumn;
    using einheit::cli::render::AddRow;
    using einheit::cli::render::Align;
    using einheit::cli::render::Cell;
    using einheit::cli::render::Priority;
    using einheit::cli::render::RenderFormatted;
    using einheit::cli::render::Semantic;

    // Each event carries one sample for one metric. We keep a
    // bounded rolling buffer per metric (mutable because the
    // contract has RenderEvent as const).
    std::lock_guard<std::mutex> lk(series_mu_);
    const auto name =
        topic.starts_with("state.metrics.")
            ? topic.substr(14)
            : topic;
    try {
      const std::string text(event.data.begin(), event.data.end());
      const auto value = std::stod(text);
      auto &series = series_[name];
      series.push_back(value);
      const std::size_t kWindow = 32;
      if (series.size() > kWindow) {
        series.erase(series.begin(),
                     series.begin() +
                         (series.size() - kWindow));
      }
    } catch (...) {
      return;
    }

    // Render a fresh table over the rolling state.
    einheit::cli::render::Table t;
    AddColumn(t, "metric", Align::Left, Priority::High);
    AddColumn(t, "last", Align::Right, Priority::High);
    AddColumn(t, "trend", Align::Left, Priority::High);
    std::vector<std::string> keys;
    keys.reserve(series_.size());
    for (const auto &[k, _] : series_) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    for (const auto &k : keys) {
      const auto &s = series_.at(k);
      if (s.empty()) continue;
      AddRow(t, {
          Cell{k, Semantic::Emphasis},
          Cell{std::format("{:.0f}", s.back()),
               Semantic::Info},
          Cell{einheit::cli::render::Sparkline(s, renderer.Caps()),
               Semantic::Good},
      });
    }
    RenderFormatted(t, renderer);
  }

 private:
  std::shared_ptr<Schema> schema_;
  mutable std::mutex series_mu_;
  mutable std::unordered_map<std::string, std::vector<double>>
      series_;
};

}  // namespace

auto NewExampleAdapter()
    -> std::unique_ptr<einheit::cli::ProductAdapter> {
  return std::make_unique<ExampleAdapter>();
}

}  // namespace einheit::adapters::example
