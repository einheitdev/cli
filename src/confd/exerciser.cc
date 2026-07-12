/// @file exerciser.cc
/// @brief Schema-driven config exerciser.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/exerciser.h"

#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <variant>

namespace einheit::cli::confd {
namespace {

auto Skipped(const std::string &path,
             const std::vector<std::string> &prefixes) -> bool {
  for (const auto &p : prefixes) {
    if (path == p || path.rfind(p + ".", 0) == 0) return true;
  }
  return false;
}

auto AddPrimitiveCases(const std::string &path,
                       const schema::PrimitiveSpec &spec,
                       std::vector<ExerciseCase> &out) -> void {
  using schema::PrimitiveType;
  switch (spec.type) {
    case PrimitiveType::String:
      out.push_back({path, "exerciser-string", true, "string"});
      break;
    case PrimitiveType::Integer: {
      std::int64_t lo = 0;
      std::int64_t hi = 65535;
      if (spec.range) {
        lo = spec.range->first;
        hi = spec.range->second;
      }
      out.push_back({path, std::to_string(lo), true, "range min"});
      out.push_back({path, std::to_string(hi), true, "range max"});
      out.push_back({path, std::to_string(lo + (hi - lo) / 2), true,
                     "range mid"});
      if (spec.range) {
        if (lo > std::numeric_limits<std::int64_t>::min()) {
          out.push_back({path, std::to_string(lo - 1), false,
                         "below range"});
        }
        if (hi < std::numeric_limits<std::int64_t>::max()) {
          out.push_back({path, std::to_string(hi + 1), false,
                         "above range"});
        }
      }
      out.push_back({path, "notanumber", false, "non-numeric"});
      break;
    }
    case PrimitiveType::Boolean:
      out.push_back({path, "true", true, "bool true"});
      out.push_back({path, "false", true, "bool false"});
      out.push_back({path, "maybe", false, "bad bool"});
      break;
    case PrimitiveType::Cidr:
      out.push_back({path, "192.0.2.0/24", true, "cidr"});
      out.push_back({path, "not-a-cidr", false, "bad cidr"});
      out.push_back({path, "192.0.2.0/33", false,
                     "cidr prefix out of range"});
      break;
    case PrimitiveType::IpAddress:
      out.push_back({path, "192.0.2.7", true, "ip"});
      out.push_back({path, "999.9.9.9", false, "bad ip octet"});
      out.push_back({path, "not.an.ip", false, "non-ip"});
      break;
    case PrimitiveType::EnumStr:
      for (const auto &v : spec.values) {
        out.push_back({path, v, true,
                       std::format("enum member '{}'", v)});
      }
      out.push_back({path, "not-a-member-zz", false, "bad enum"});
      break;
  }
}

auto WalkNode(const std::string &path, const schema::Node &node,
              const ExerciseOptions &opts,
              std::vector<ExerciseCase> &out) -> void;

auto WalkObject(const std::string &prefix,
                const schema::ObjectSpec &obj,
                const ExerciseOptions &opts,
                std::vector<ExerciseCase> &out) -> void {
  for (const auto &[name, child] : obj.fields) {
    if (!child) continue;
    const auto path =
        prefix.empty() ? name : prefix + "." + name;
    WalkNode(path, *child, opts, out);
  }
}

auto WalkNode(const std::string &path, const schema::Node &node,
              const ExerciseOptions &opts,
              std::vector<ExerciseCase> &out) -> void {
  if (Skipped(path, opts.skip_prefixes)) return;
  if (const auto *prim =
          std::get_if<schema::PrimitiveSpec>(&node.shape)) {
    AddPrimitiveCases(path, *prim, out);
  } else if (const auto *obj =
                 std::get_if<schema::ObjectSpec>(&node.shape)) {
    WalkObject(path, *obj, opts, out);
  } else if (const auto *map =
                 std::get_if<schema::MapSpec>(&node.shape)) {
    if (!map->value) return;
    std::string key;
    if (const auto it = opts.map_keys.find(path);
        it != opts.map_keys.end()) {
      key = it->second;
    } else {
      key = map->key_type == schema::PrimitiveType::Integer
                ? "1"
                : "k1";
    }
    WalkNode(path + "." + key, *map->value, opts, out);
  }
  // Lists and custom types are skipped: no shipping schema uses
  // lists yet, and custom validators are adapter-owned.
}

/// Minimal wire-request factory for driving HandleRequest directly.
auto Req(std::string command, std::vector<std::string> args,
         const std::string &session) -> protocol::Request {
  protocol::Request r;
  r.id = "exerciser";
  r.user = "exerciser";
  r.role = "admin";
  r.command = std::move(command);
  r.args = std::move(args);
  if (!session.empty()) r.session_id = session;
  return r;
}

auto Body(const protocol::Response &r) -> std::string {
  return {r.data.begin(), r.data.end()};
}

auto Ok(const protocol::Response &r) -> bool {
  return r.status == protocol::ResponseStatus::Ok;
}

/// Open a configure session, returning its id via HandleConfigure's
/// response body.
auto OpenSession(Runtime &rt, std::string &session,
                 std::string &err) -> bool {
  const auto resp = rt.HandleRequest(Req("configure", {}, ""));
  if (!Ok(resp)) {
    err = std::format("configure failed: {}",
                      resp.error ? resp.error->message : "?");
    return false;
  }
  session = Body(resp);
  return true;
}

auto RunningEquals(const Config &a, const Config &b,
                   std::string &diff) -> bool {
  for (const auto &[k, v] : a) {
    const auto it = b.find(k);
    if (it == b.end()) {
      diff = std::format("'{}' missing (was '{}')", k, v);
      return false;
    }
    if (it->second != v) {
      diff = std::format("'{}' is '{}', expected '{}'", k,
                         it->second, v);
      return false;
    }
  }
  for (const auto &[k, v] : b) {
    if (!a.contains(k)) {
      diff = std::format("unexpected '{}' = '{}'", k, v);
      return false;
    }
  }
  return true;
}

}  // namespace

auto GenerateCases(const schema::Schema &schema,
                   const ExerciseOptions &opts)
    -> std::vector<ExerciseCase> {
  std::vector<ExerciseCase> out;
  WalkObject("", schema.root, opts, out);
  return out;
}

auto ExerciseRuntime(Runtime &rt,
                     const std::vector<ExerciseCase> &cases)
    -> std::vector<ExerciseFailure> {
  std::vector<ExerciseFailure> failures;
  const auto fail = [&failures](const ExerciseCase &c,
                                std::string detail) {
    failures.push_back({c, std::move(detail)});
  };

  // `rollback previous` needs a predecessor; anchor the current
  // running config as commit #1 when history is empty.
  if (rt.HistorySize() == 0) {
    std::string session;
    std::string err;
    if (!OpenSession(rt, session, err)) return {{{}, err}};
    if (!Ok(rt.HandleRequest(Req("commit", {}, session)))) {
      return {{{}, "anchor commit failed"}};
    }
  }

  for (const auto &c : cases) {
    const auto baseline = rt.Running();
    std::string session;
    std::string err;
    if (!OpenSession(rt, session, err)) {
      fail(c, err);
      continue;
    }
    const auto set_resp =
        rt.HandleRequest(Req("set", {c.path, c.value}, session));

    if (!c.valid) {
      // Rejection may come at set (schema) or commit (backend);
      // either way running state must be untouched afterwards.
      if (Ok(set_resp)) {
        const auto commit_resp =
            rt.HandleRequest(Req("commit", {}, session));
        if (Ok(commit_resp)) {
          fail(c, "invalid value was committed");
        }
      }
      (void)rt.HandleRequest(Req("rollback", {}, session));
      std::string diff;
      if (!RunningEquals(baseline, rt.Running(), diff)) {
        fail(c, std::format("running changed after reject: {}",
                            diff));
      }
      continue;
    }

    if (!Ok(set_resp)) {
      fail(c, std::format("set rejected: {}",
                          set_resp.error ? set_resp.error->message
                                          : "?"));
      (void)rt.HandleRequest(Req("rollback", {}, session));
      continue;
    }
    const bool is_change = !baseline.contains(c.path) ||
                           baseline.at(c.path) != c.value;
    if (is_change) {
      const auto diff_resp =
          rt.HandleRequest(Req("show_diff", {}, session));
      if (Body(diff_resp).find(c.path + "=") == std::string::npos) {
        fail(c, "pending change missing from show diff");
      }
    }
    const auto commit_resp =
        rt.HandleRequest(Req("commit", {}, session));
    if (!Ok(commit_resp)) {
      fail(c, std::format("commit failed: {}",
                          commit_resp.error
                              ? commit_resp.error->message
                              : "?"));
      (void)rt.HandleRequest(Req("rollback", {}, session));
      continue;
    }
    auto running = rt.Running();
    if (!running.contains(c.path) ||
        running.at(c.path) != c.value) {
      fail(c, "committed value not in running config");
    }
    auto expected = baseline;
    expected[c.path] = c.value;
    std::string diff;
    if (!RunningEquals(expected, running, diff)) {
      fail(c, std::format("commit touched other paths: {}", diff));
    }
    const auto rb_resp =
        rt.HandleRequest(Req("rollback_previous", {}, ""));
    if (!Ok(rb_resp)) {
      fail(c, std::format("rollback previous failed: {}",
                          rb_resp.error ? rb_resp.error->message
                                         : "?"));
      continue;
    }
    if (!RunningEquals(baseline, rt.Running(), diff)) {
      fail(c, std::format("rollback did not restore: {}", diff));
    }
  }
  return failures;
}

}  // namespace einheit::cli::confd
