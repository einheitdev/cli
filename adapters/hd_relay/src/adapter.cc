/// @file adapter.cc
/// @brief hd-relay adapter implementation.
///
/// Declarative: schema loaded from baked YAML, command specs
/// mirror the daemon's REST surface, renderers decode MessagePack
/// response bodies the daemon ships back. Every wire_command here
/// is expected to have a matching handler on the Hyper-DERP side
/// (Stage 2 of the adapter rollout).
// Copyright (c) 2026 Einheit Networks

#include "adapters/hd_relay/adapter.h"

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

namespace einheit::adapters::hd_relay {
namespace {

using einheit::cli::ArgSpec;
using einheit::cli::CommandSpec;
using einheit::cli::ProductAdapter;
using einheit::cli::ProductMetadata;
using einheit::cli::RoleGate;
using einheit::cli::protocol::Event;
using einheit::cli::protocol::Response;
using einheit::cli::render::AddColumn;
using einheit::cli::render::AddRow;
using einheit::cli::render::Align;
using einheit::cli::render::Cell;
using einheit::cli::render::Priority;
using einheit::cli::render::RenderError;
using einheit::cli::render::RenderFormatted;
using einheit::cli::render::Renderer;
using einheit::cli::render::Semantic;
using einheit::cli::render::Sparkline;
using einheit::cli::render::Table;
using einheit::cli::schema::Schema;

// Baked schema — kept in lockstep with adapters/hd_relay/schema.yaml.
// Compiled into the binary so the adapter boots without a separate
// config file on appliance installs.
constexpr const char *kSchemaYaml = R"YAML(
version: 1
product: hd-relay

config:
  port:
    type: integer
    range: [1, 65535]
    default: "3340"
    help: "DERP / HD listen port"
  workers:
    type: integer
    range: [0, 32]
    default: "0"
    help: "Data-plane worker threads (0 = hardware_concurrency)"
  pin_cores:
    type: list
    item:
      type: integer
      range: [0, 255]
    help: "CPU cores to pin workers to (optional)"
  sqpoll:
    type: boolean
    default: "false"
    help: "Enable io_uring SQPOLL (requires CAP_SYS_NICE)"
  sockbuf:
    type: integer
    range: [0, 268435456]
    default: "0"
    help: "Per-socket SO_SNDBUF/SO_RCVBUF in bytes (0 = OS default)"
  max_accept_rate:
    type: integer
    range: [0, 1000000]
    default: "0"
    help: "Accepted connections per second cap (0 = unlimited)"
  tls_cert:
    type: string
    help: "Path to kTLS certificate (PEM)"
    example: "/etc/hyper-derp/cert.pem"
  tls_key:
    type: string
    help: "Path to kTLS private key (PEM)"
    example: "/etc/hyper-derp/key.pem"
  log_level:
    type: enum
    values: [debug, info, warn, error]
    default: "info"
    help: "spdlog level"
  metrics:
    type: object
    fields:
      port:
        type: integer
        range: [0, 65535]
        default: "0"
        help: "Metrics HTTP port (0 = disabled)"
      tls_cert:
        type: string
        help: "Metrics HTTPS certificate (optional)"
      tls_key:
        type: string
        help: "Metrics HTTPS key (optional)"
      debug_endpoints:
        type: boolean
        default: "false"
        help: "Expose /debug/workers and /debug/peers"
  hd:
    type: object
    fields:
      relay_key:
        type: string
        help: "rk_<64 hex> or raw 64-char hex — HD enrollment secret"
        example: "rk_aabbcc..."
      relay_id:
        type: integer
        range: [0, 65535]
        default: "0"
        help: "Fleet identifier for this relay (0 = standalone)"
      enroll_mode:
        type: enum
        values: [manual, auto]
        default: "manual"
        help: "Accept new peers manually via admin API or auto-approve"
      seed_relays:
        type: list
        item:
          type: string
        help: "host:port seed entries for fleet bootstrap"
      denylist_path:
        type: string
        help: "Persistent revoked-key file (empty = in-memory only)"
      peer_policy_path:
        type: string
        help: "Persistent per-peer routing-policy file"
      fleet_policy_path:
        type: string
        help: "Static fleet policy YAML (superseded by fleet_controller)"
      audit_log_path:
        type: string
        help: "Routing-policy audit log file (LD-JSON)"
      audit_log_max_bytes:
        type: integer
        range: [0, 2000000000]
        default: "100000000"
        help: "Rotate audit log at this size"
      audit_log_keep:
        type: integer
        range: [1, 100]
        default: "10"
        help: "Rotated audit-log files to retain (.1..N)"
      enrollment:
        type: object
        fields:
          max_peers:
            type: integer
            range: [0, 100000]
            help: "Auto-approve cap"
          require_ip_range:
            type: string
            help: "CIDR restricting auto-approval (IPv4)"
            example: "10.0.0.0/8"
          allowed_keys:
            type: list
            item:
              type: string
            help: "Glob patterns matched against ck_ prefixed keys"
      relay_policy:
        type: object
        fields:
          forbid_direct:
            type: boolean
            default: "false"
            help: "Deny Mode::Direct on this relay"
          forbid_relayed:
            type: boolean
            default: "false"
            help: "Deny Mode::Relayed on this relay"
          max_direct_peers:
            type: integer
            range: [0, 1000000]
            default: "0"
            help: "Concurrent-direct-tunnel cap (0 = unlimited)"
          audit_relayed_traffic:
            type: boolean
            default: "true"
            help: "Log relayed decisions to the audit log"
          default_mode:
            type: enum
            values:
              [prefer_direct, require_direct,
               prefer_relay, require_relay]
            default: "prefer_direct"
            help: "Fallback intent when no peer/client pin applies"
      federation:
        type: object
        fields:
          fleet_id:
            type: string
            help: "This relay's fleet id"
          accept_from:
            type: list
            item:
              type: object
              fields:
                fleet_id:
                  type: string
                  help: "Remote fleet id glob (trailing * allowed)"
                allowed_destinations:
                  type: list
                  item:
                    type: string
                  help: "Target-key globs (ck_... or raw hex)"
          reject_from:
            type: list
            item:
              type: string
            help: "Hard-deny fleet-id globs; checked first"
      fleet_controller:
        type: object
        fields:
          url:
            type: string
            help: "HTTPS URL of the policy server"
            example: "https://fleet.example.com"
          signing_pubkey_b64:
            type: string
            help: "Ed25519 bundle-verify public key (base64)"
          client_cert:
            type: string
            help: "mTLS client certificate (PEM)"
          client_key:
            type: string
            help: "mTLS client key (PEM)"
          ca_bundle:
            type: string
            help: "CA bundle for server-cert verification"
          poll_interval_secs:
            type: integer
            range: [10, 3600]
            default: "60"
          bundle_cache_path:
            type: string
            help: "Last-known-good bundle cache path"
  level2:
    type: object
    fields:
      enabled:
        type: boolean
        default: "false"
      stun_port:
        type: integer
        range: [1, 65535]
        default: "3478"
      xdp_interface:
        type: string
        help: "NIC to attach the XDP program to (e.g. eth0)"
      turn:
        type: object
        fields:
          realm:
            type: string
          max_allocations:
            type: integer
            range: [0, 1000000]
            default: "10000"
          default_lifetime:
            type: integer
            range: [0, 86400]
            default: "600"
  peer_rate_limit:
    type: integer
    range: [0, 10000000000]
    default: "0"
    help: "Per-peer recv byte/s rate limit (0 = unlimited)"

types: {}
)YAML";

auto LoadBakedSchema() -> std::shared_ptr<Schema> {
  const auto path =
      std::filesystem::temp_directory_path() /
      "einheit_hd_relay_schema.yaml";
  {
    std::ofstream f(path);
    f << kSchemaYaml;
  }
  auto s = einheit::cli::schema::LoadSchema(path.string());
  if (!s) return std::make_shared<Schema>();
  return *s;
}

// -- Command builder helpers -----------------------------------------------

auto Show(std::string path, std::string wire, std::string help,
          RoleGate role = RoleGate::AnyAuthenticated)
    -> CommandSpec {
  CommandSpec c;
  c.path = "show " + std::move(path);
  c.wire_command = std::move(wire);
  c.help = std::move(help);
  c.role = role;
  return c;
}

auto Peer(std::string path, std::string wire, std::string help,
          std::vector<ArgSpec> args = {},
          RoleGate role = RoleGate::AdminOnly) -> CommandSpec {
  CommandSpec c;
  c.path = "peer " + std::move(path);
  c.wire_command = std::move(wire);
  c.help = std::move(help);
  c.args = std::move(args);
  c.role = role;
  return c;
}

auto Watch(std::string path, std::string help) -> CommandSpec {
  CommandSpec c;
  c.path = "watch " + std::move(path);
  c.wire_command = "";  // framework-local; purely subscription
  c.help = std::move(help);
  c.role = RoleGate::AnyAuthenticated;
  return c;
}

// -- Payload-decode helpers ------------------------------------------------
//
// Hyper-DERP's daemon-side einheit protocol (Stage 2) will encode
// Response::data as MessagePack blobs keyed by a `type` tag. Until
// that lands, the learning daemon echoes simple key=value text
// back. Both paths flow through ParseKvLines, which degrades
// gracefully on bad input.

struct Kv {
  std::string key;
  std::string value;
};

auto ParseKvLines(const std::vector<std::uint8_t> &data)
    -> std::vector<Kv> {
  std::vector<Kv> out;
  const std::string body(data.begin(), data.end());
  std::istringstream iss(body);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    const auto eq = line.find('=');
    Kv kv;
    if (eq == std::string::npos) {
      kv.key = line;
    } else {
      kv.key = line.substr(0, eq);
      kv.value = line.substr(eq + 1);
    }
    out.push_back(std::move(kv));
  }
  return out;
}

auto SemanticForState(const std::string &state) -> Semantic {
  if (state == "approved") return Semantic::Good;
  if (state == "pending") return Semantic::Warn;
  if (state == "denied" || state == "revoked") return Semantic::Bad;
  return Semantic::Default;
}

// -- Shared renderers ------------------------------------------------------

auto RenderKvBlock(const std::vector<Kv> &kvs, Renderer &r) -> void {
  Table t;
  AddColumn(t, "field", Align::Left, Priority::High);
  AddColumn(t, "value", Align::Left, Priority::High);
  for (const auto &kv : kvs) {
    Semantic vsem = Semantic::Default;
    if (kv.key == "state") {
      vsem = SemanticForState(kv.value);
    } else if (kv.key.ends_with("_count") ||
               kv.key.ends_with("_bytes") ||
               kv.key == "version") {
      vsem = Semantic::Info;
    } else if (kv.value.empty() || kv.value == "<none>") {
      vsem = Semantic::Dim;
    }
    AddRow(t, {Cell{kv.key, Semantic::Emphasis},
               Cell{kv.value, vsem}});
  }
  RenderFormatted(t, r);
}

auto RenderPeerTable(const std::vector<std::vector<Kv>> &rows,
                     Renderer &r) -> void {
  Table t;
  AddColumn(t, "key", Align::Left, Priority::High);
  AddColumn(t, "state", Align::Left, Priority::High);
  AddColumn(t, "policy", Align::Left, Priority::Medium);
  AddColumn(t, "rules", Align::Right, Priority::Medium);
  AddColumn(t, "fd", Align::Right, Priority::Low);
  for (const auto &row : rows) {
    std::unordered_map<std::string, std::string> m;
    for (const auto &kv : row) m[kv.key] = kv.value;
    const auto &state = m["state"];
    std::string pol = m["policy"];
    if (pol.empty()) pol = "—";
    AddRow(t, {
                  Cell{m["key"], Semantic::Default},
                  Cell{state, SemanticForState(state)},
                  Cell{pol,
                       pol == "—" ? Semantic::Dim
                                   : Semantic::Info},
                  Cell{m.count("rules") ? m["rules"] : "0",
                       Semantic::Info},
                  Cell{m.count("fd") ? m["fd"] : "",
                       Semantic::Dim},
              });
  }
  if (t.rows.empty()) {
    AddRow(t, {Cell{"no peers", Semantic::Dim},
               Cell{"", Semantic::Dim}, Cell{"", Semantic::Dim},
               Cell{"", Semantic::Dim}, Cell{"", Semantic::Dim}});
  }
  RenderFormatted(t, r);
}

// The learning daemon emits one Kv line per peer as
// `peer.<n>.<field>=<value>`. Split these into per-row groups.
auto GroupPeersByIndex(const std::vector<Kv> &kvs)
    -> std::vector<std::vector<Kv>> {
  std::unordered_map<std::string, std::vector<Kv>> groups;
  std::vector<std::string> order;
  for (const auto &kv : kvs) {
    if (!kv.key.starts_with("peer.")) continue;
    const auto rest = kv.key.substr(5);
    const auto dot = rest.find('.');
    if (dot == std::string::npos) continue;
    const auto idx = rest.substr(0, dot);
    const auto field = rest.substr(dot + 1);
    if (!groups.count(idx)) order.push_back(idx);
    groups[idx].push_back({field, kv.value});
  }
  std::vector<std::vector<Kv>> rows;
  for (const auto &idx : order) rows.push_back(groups[idx]);
  return rows;
}

// -- The adapter -----------------------------------------------------------

class HdRelayAdapter : public ProductAdapter {
 public:
  HdRelayAdapter() : schema_(LoadBakedSchema()) {}

  auto Metadata() const -> ProductMetadata override {
    ProductMetadata m;
    m.id = "hd-relay";
    m.display_name = "Hyper-DERP Relay";
    m.version = "0.1.0";
    m.banner =
        "einheit — hd-relay "
        "(DERP/HD relay, routing policy, fleet control)";
    m.prompt = "hd-relay";
    return m;
  }

  auto GetSchema() const -> const Schema & override {
    return *schema_;
  }

  auto ControlSocketPath() const -> std::string override {
    return "ipc:///var/run/einheit/hd-relay.ctl";
  }

  auto EventSocketPath() const -> std::string override {
    return "ipc:///var/run/einheit/hd-relay.pub";
  }

  auto Commands() const -> std::vector<CommandSpec> override {
    std::vector<CommandSpec> out;

    // -- show --
    out.push_back(Show("status", "show_status",
                       "Relay + fleet + peer summary"));
    out.push_back(Show("peers", "show_peers",
                       "List HD peers"));
    {
      auto c = Show(
          "peer", "show_peer",
          "Show one peer's detail + policy + rules");
      c.args = {ArgSpec{"key", "ck_ prefixed or raw-hex peer key",
                         true, ""}};
      out.push_back(std::move(c));
    }
    out.push_back(Show("audit", "show_audit",
                       "Recent routing-policy decisions",
                       RoleGate::OperatorOrAdmin));
    out.push_back(Show("counters", "show_counters",
                       "Per-worker + aggregate counters",
                       RoleGate::OperatorOrAdmin));
    out.push_back(Show("config", "show_config",
                       "Redacted runtime configuration",
                       RoleGate::OperatorOrAdmin));
    out.push_back(Show("fleet", "show_fleet",
                       "Fleet controller + relay table state",
                       RoleGate::OperatorOrAdmin));

    // -- peer lifecycle --
    out.push_back(Peer("approve", "peer_approve",
                       "Approve a pending peer",
                       {ArgSpec{"key", "peer key", true, ""}}));
    out.push_back(Peer("deny", "peer_deny",
                       "Deny a pending peer",
                       {ArgSpec{"key", "peer key", true, ""}}));
    out.push_back(Peer("revoke", "peer_revoke",
                       "Revoke (denylist + disconnect) a peer",
                       {ArgSpec{"key", "peer key", true, ""}}));
    {
      auto c = Peer(
          "redirect", "peer_redirect",
          "Send a Redirect frame (0x22) to a peer",
          {ArgSpec{"key", "peer key", true, ""},
           ArgSpec{"target_url", "hd://host:port", true, ""}});
      c.flags = {"reason"};
      out.push_back(std::move(c));
    }

    // -- peer policy --
    {
      auto c = Peer(
          "policy set", "peer_policy_set",
          "Pin per-peer routing policy",
          {ArgSpec{"key", "peer key", true, ""}});
      c.flags = {"intent", "override", "audit-tag", "reason"};
      out.push_back(std::move(c));
    }
    out.push_back(
        Peer("policy clear", "peer_policy_clear",
             "Reset a peer's policy to defaults",
             {ArgSpec{"key", "peer key", true, ""}}));
    {
      auto c = Peer(
          "rule add", "peer_rule_add",
          "Add a forwarding rule to a peer",
          {ArgSpec{"key", "source peer key", true, ""},
           ArgSpec{"dst", "destination peer key", true, ""}});
      out.push_back(std::move(c));
    }

    // -- relay-scoped admin --
    {
      CommandSpec c;
      c.path = "relay init";
      c.wire_command = "relay_init";
      c.help =
          "Generate a fresh rk_ relay key (one-shot, not stored)";
      c.role = RoleGate::AdminOnly;
      out.push_back(std::move(c));
    }

    // -- watch (framework-local; no wire RPC) --
    out.push_back(
        Watch("peers", "Live peer table"));
    out.push_back(
        Watch("audit", "Stream routing-policy decisions"));
    out.push_back(
        Watch("metrics", "Stream per-worker metrics"));

    return out;
  }

  auto RenderResponse(const CommandSpec &cmd,
                      const Response &response,
                      Renderer &renderer) const -> void override {
    if (response.error) {
      std::string msg = response.error->message;
      std::string hint = response.error->hint;
      if (hint.empty()) {
        if (const auto pos = msg.find(" — ");
            pos != std::string::npos) {
          hint = msg.substr(pos + 5);
          msg = msg.substr(0, pos);
        }
      }
      RenderError(response.error->code, msg, hint, renderer);
      return;
    }

    // Empty Response body — daemon handled the command with no
    // payload (e.g. peer_approve). Render a single-cell "ok" so
    // operators get feedback.
    if (response.data.empty()) {
      Table t;
      AddColumn(t, "status", Align::Left, Priority::High);
      AddRow(t, {Cell{"ok", Semantic::Good}});
      RenderFormatted(t, renderer);
      return;
    }

    const auto kvs = ParseKvLines(response.data);
    const auto &path = cmd.path;

    if (path == "show peers") {
      const auto rows = GroupPeersByIndex(kvs);
      RenderPeerTable(rows, renderer);
      return;
    }
    if (path == "show audit") {
      RenderAuditTable(kvs, renderer);
      return;
    }
    if (path == "show counters") {
      RenderCountersTable(kvs, renderer);
      return;
    }
    // Default: key=value list for show_status / show_peer /
    // show_config / show_fleet / relay_init / etc.
    RenderKvBlock(kvs, renderer);
  }

  auto EventTopicsFor(const CommandSpec &cmd) const
      -> std::vector<std::string> override {
    if (cmd.path == "watch peers") {
      return {"state.peers."};
    }
    if (cmd.path == "watch audit") {
      return {"state.audit."};
    }
    if (cmd.path == "watch metrics") {
      return {"state.metrics."};
    }
    return {};
  }

  auto RenderEvent(const std::string &topic, const Event &event,
                   Renderer &renderer) const -> void override {
    if (topic.starts_with("state.metrics.")) {
      RenderMetricEvent(topic, event, renderer);
      return;
    }
    if (topic.starts_with("state.peers.") ||
        topic.starts_with("state.audit.")) {
      // For the in-process learning daemon we render each delivered
      // event as a short key=value block. The Stage-2 daemon will
      // ship richer msgpack bodies; when that lands the renderer
      // detects shape via a first-byte tag.
      Table t;
      AddColumn(t, "topic", Align::Left, Priority::High);
      AddColumn(t, "body", Align::Left, Priority::High);
      std::string body(event.data.begin(), event.data.end());
      if (body.empty()) body = "(empty)";
      AddRow(t, {Cell{topic, Semantic::Info},
                 Cell{body, Semantic::Default}});
      RenderFormatted(t, renderer);
      return;
    }
  }

 private:
  auto RenderAuditTable(const std::vector<Kv> &kvs,
                        Renderer &r) const -> void {
    // Learning daemon shape: audit.<n>.<field>=<value>.
    std::unordered_map<std::string, std::vector<Kv>> groups;
    std::vector<std::string> order;
    for (const auto &kv : kvs) {
      if (!kv.key.starts_with("audit.")) continue;
      const auto rest = kv.key.substr(6);
      const auto dot = rest.find('.');
      if (dot == std::string::npos) continue;
      const auto idx = rest.substr(0, dot);
      const auto field = rest.substr(dot + 1);
      if (!groups.count(idx)) order.push_back(idx);
      groups[idx].push_back({field, kv.value});
    }
    Table t;
    AddColumn(t, "ts", Align::Left, Priority::Medium);
    AddColumn(t, "client", Align::Left, Priority::High);
    AddColumn(t, "target", Align::Left, Priority::High);
    AddColumn(t, "intent", Align::Left, Priority::Medium);
    AddColumn(t, "decision", Align::Left, Priority::High);
    AddColumn(t, "reason", Align::Left, Priority::Low);
    for (const auto &idx : order) {
      std::unordered_map<std::string, std::string> m;
      for (const auto &kv : groups[idx]) m[kv.key] = kv.value;
      Semantic dec = Semantic::Default;
      if (m["decision"] == "direct") dec = Semantic::Good;
      else if (m["decision"] == "relayed") dec = Semantic::Info;
      else if (m["decision"] == "denied") dec = Semantic::Bad;
      AddRow(t, {Cell{m["ts"], Semantic::Dim},
                 Cell{m["client"], Semantic::Default},
                 Cell{m["target"], Semantic::Default},
                 Cell{m["intent"], Semantic::Info},
                 Cell{m["decision"], dec},
                 Cell{m["reason"],
                      m["reason"].empty() ? Semantic::Dim
                                           : Semantic::Warn}});
    }
    if (t.rows.empty()) {
      AddRow(t, {Cell{"no decisions yet", Semantic::Dim},
                 Cell{"", Semantic::Dim},
                 Cell{"", Semantic::Dim},
                 Cell{"", Semantic::Dim},
                 Cell{"", Semantic::Dim},
                 Cell{"", Semantic::Dim}});
    }
    RenderFormatted(t, r);
  }

  auto RenderCountersTable(const std::vector<Kv> &kvs,
                           Renderer &r) const -> void {
    Table t;
    AddColumn(t, "counter", Align::Left, Priority::High);
    AddColumn(t, "value", Align::Right, Priority::High);
    for (const auto &kv : kvs) {
      AddRow(t, {Cell{kv.key, Semantic::Emphasis},
                 Cell{kv.value, Semantic::Info}});
    }
    if (t.rows.empty()) {
      AddRow(t, {Cell{"(no counters)", Semantic::Dim},
                 Cell{"", Semantic::Dim}});
    }
    RenderFormatted(t, r);
  }

  auto RenderMetricEvent(const std::string &topic, const Event &event,
                         Renderer &renderer) const -> void {
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

    Table t;
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
                    Cell{Sparkline(s, renderer.Caps()),
                         Semantic::Good},
                });
    }
    RenderFormatted(t, renderer);
  }

  std::shared_ptr<Schema> schema_;
  mutable std::mutex series_mu_;
  mutable std::unordered_map<std::string, std::vector<double>>
      series_;
};

}  // namespace

auto NewHdRelayAdapter()
    -> std::unique_ptr<einheit::cli::ProductAdapter> {
  return std::make_unique<HdRelayAdapter>();
}

}  // namespace einheit::adapters::hd_relay
