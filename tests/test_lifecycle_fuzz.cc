/// @file test_lifecycle_fuzz.cc
/// @brief Model-based fuzzing of the confd lifecycle state machine.
///
/// Random-but-deterministic sequences of configure / set / delete /
/// commit / rollback (candidate, previous, to <id>) / process
/// restart run against a real Runtime + MemoryBackend and are
/// checked op-by-op against a ~60-line reference model of what
/// running config and history must be. This is the layer that
/// catches state-machine-over-time bugs (the stale-persisted-
/// running reconcile bug was exactly this class): example-based
/// tests never think to write the sequence that breaks.
/// commit-confirmed is excluded — its timer semantics don't fuzz
/// on a unit-test clock and have their own suite.
// Copyright (c) 2026 Einheit Networks

#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/confd/memory_backend.h"
#include "einheit/cli/confd/runtime.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {
namespace {

namespace fs = std::filesystem;

constexpr const char *kSchemaYaml = R"yaml(
version: 1
product: lifecycle-fuzz

config:
  name:
    type: string
    help: "Free string"
  port:
    type: integer
    range: [1, 100]
    help: "Small int"
  mode:
    type: enum
    values: [alpha, beta, gamma]
    help: "Enum"
  nets:
    type: map
    key: string
    value:
      type: object
      fields:
        addr:
          type: cidr
          help: "Address"

types: {}
)yaml";

/// A settable path plus its valid/invalid value pools.
struct PathPool {
  std::string path;
  std::vector<std::string> valid;
  std::vector<std::string> invalid;
};

const std::vector<PathPool> kPools = {
    {"name", {"one", "two", "three"}, {}},
    {"port", {"1", "50", "100"}, {"0", "101", "nope"}},
    {"mode", {"alpha", "beta", "gamma"}, {"delta"}},
    {"nets.n1.addr",
     {"192.0.2.0/24", "198.51.100.0/25"},
     {"not-a-cidr"}},
    {"nets.n2.addr", {"203.0.113.0/24"}, {"203.0.113.0/99"}},
};

/// Reference model of the runtime's externally observable state.
struct Model {
  Config running;
  std::vector<Config> history;
  bool session_open = false;
  Config candidate;
};

auto Req(std::string command, std::vector<std::string> args,
         const std::string &session) -> protocol::Request {
  protocol::Request r;
  r.id = "fuzz";
  r.user = "fuzz";
  r.role = "admin";
  r.command = std::move(command);
  r.args = std::move(args);
  if (!session.empty()) r.session_id = session;
  return r;
}

auto Ok(const protocol::Response &r) -> bool {
  return r.status == protocol::ResponseStatus::Ok;
}

auto Body(const protocol::Response &r) -> std::string {
  return {r.data.begin(), r.data.end()};
}

auto DiffConfigs(const Config &want, const Config &got)
    -> std::string {
  for (const auto &[k, v] : want) {
    const auto it = got.find(k);
    if (it == got.end()) {
      return std::format("'{}' missing (want '{}')", k, v);
    }
    if (it->second != v) {
      return std::format("'{}' = '{}', want '{}'", k, it->second,
                         v);
    }
  }
  for (const auto &[k, v] : got) {
    if (!want.contains(k)) {
      return std::format("unexpected '{}' = '{}'", k, v);
    }
  }
  return {};
}

auto RunSeed(std::uint32_t seed) -> void {
  const auto state_dir =
      fs::temp_directory_path() /
      std::format("confd-fuzz-{}-{}", ::getpid(), seed);
  fs::remove_all(state_dir);

  auto schema = schema::LoadSchemaFromString(kSchemaYaml);
  ASSERT_TRUE(schema.has_value());
  MemoryBackend backend(*schema);
  RuntimeOptions opts;
  opts.state_dir = state_dir.string();
  std::optional<Runtime> rt;
  rt.emplace(backend, opts);

  Model model;
  std::string session;
  // Commit ids as the runtime assigned them, parallel to
  // model.history.
  std::vector<std::uint64_t> ids;

  std::mt19937 rng(seed);
  const auto pick = [&rng](std::size_t n) -> std::size_t {
    return rng() % n;
  };

  const auto parse_commit_id =
      [](const std::string &body) -> std::uint64_t {
    const auto pos = body.find("commit_id=");
    if (pos == std::string::npos) return 0;
    return std::stoull(body.substr(pos + 10));
  };

  for (int op = 0; op < 400; ++op) {
    const auto roll = rng() % 100;
    std::string what;

    if (roll < 15) {
      // configure — accepted iff no session is open.
      what = "configure";
      const auto resp = rt->HandleRequest(Req("configure", {}, ""));
      ASSERT_EQ(Ok(resp), !model.session_open)
          << std::format("op {} {}: seed {}", op, what, seed);
      if (Ok(resp)) {
        session = Body(resp);
        model.session_open = true;
        model.candidate = model.running;
      }
    } else if (roll < 45) {
      // set — valid or (10% of the time) invalid value.
      const auto &pool = kPools[pick(kPools.size())];
      const bool try_invalid =
          !pool.invalid.empty() && (rng() % 10 == 0);
      const auto &value =
          try_invalid ? pool.invalid[pick(pool.invalid.size())]
                       : pool.valid[pick(pool.valid.size())];
      what = std::format("set {} {}", pool.path, value);
      const auto resp = rt->HandleRequest(
          Req("set", {pool.path, value}, session));
      const bool expect_ok = model.session_open && !try_invalid;
      ASSERT_EQ(Ok(resp), expect_ok)
          << std::format("op {} {}: seed {} ({})", op, what, seed,
                         resp.error ? resp.error->message : "ok");
      if (Ok(resp)) model.candidate[pool.path] = value;
    } else if (roll < 55) {
      // delete — silently erases from the candidate.
      const auto &pool = kPools[pick(kPools.size())];
      what = std::format("delete {}", pool.path);
      const auto resp =
          rt->HandleRequest(Req("delete", {pool.path}, session));
      ASSERT_EQ(Ok(resp), model.session_open)
          << std::format("op {} {}: seed {}", op, what, seed);
      if (Ok(resp)) model.candidate.erase(pool.path);
    } else if (roll < 70) {
      // commit.
      what = "commit";
      const auto resp = rt->HandleRequest(Req("commit", {}, session));
      ASSERT_EQ(Ok(resp), model.session_open)
          << std::format("op {} {}: seed {} ({})", op, what, seed,
                         resp.error ? resp.error->message : "ok");
      if (Ok(resp)) {
        model.running = model.candidate;
        model.history.push_back(model.candidate);
        ids.push_back(parse_commit_id(Body(resp)));
        model.session_open = false;
        session.clear();
      }
    } else if (roll < 75) {
      // rollback candidate — always accepted, discards any session.
      what = "rollback candidate";
      const auto resp =
          rt->HandleRequest(Req("rollback", {}, session));
      ASSERT_TRUE(Ok(resp))
          << std::format("op {} {}: seed {}", op, what, seed);
      model.session_open = false;
      session.clear();
    } else if (roll < 82) {
      // rollback previous — needs at least two commits; re-applies
      // history[n-2] and records it as a NEW commit.
      what = "rollback previous";
      const auto resp =
          rt->HandleRequest(Req("rollback_previous", {}, ""));
      const bool expect_ok = model.history.size() >= 2;
      ASSERT_EQ(Ok(resp), expect_ok)
          << std::format("op {} {}: seed {}", op, what, seed);
      if (Ok(resp)) {
        const auto target =
            model.history[model.history.size() - 2];
        model.running = target;
        model.history.push_back(target);
        ids.push_back(parse_commit_id(Body(resp)));
      }
    } else if (roll < 90) {
      // rollback to <id> — an existing id, or (sometimes) a bogus
      // one that must be rejected.
      const bool bogus = ids.empty() || (rng() % 5 == 0);
      const std::uint64_t id =
          bogus ? 999999 : ids[pick(ids.size())];
      what = std::format("rollback to {}", id);
      const auto resp = rt->HandleRequest(
          Req("rollback_to", {std::to_string(id)}, ""));
      ASSERT_EQ(Ok(resp), !bogus)
          << std::format("op {} {}: seed {}", op, what, seed);
      if (Ok(resp)) {
        std::size_t idx = 0;
        for (std::size_t i = 0; i < ids.size(); ++i) {
          if (ids[i] == id) idx = i;
        }
        const auto target = model.history[idx];
        model.running = target;
        model.history.push_back(target);
        ids.push_back(parse_commit_id(Body(resp)));
      }
    } else {
      // Process restart: durable state must survive, the open
      // session must not, and startup reconciliation must leave
      // running matching the box exactly.
      what = "restart";
      rt.reset();
      rt.emplace(backend, opts);
      model.session_open = false;
      session.clear();
    }

    const auto diff = DiffConfigs(model.running, rt->Running());
    ASSERT_TRUE(diff.empty()) << std::format(
        "op {} ({}): seed {}: running diverged: {}", op, what,
        seed, diff);
    ASSERT_EQ(rt->HistorySize(), model.history.size())
        << std::format("op {} ({}): seed {}", op, what, seed);
  }

  rt.reset();
  fs::remove_all(state_dir);
}

TEST(LifecycleFuzz, Seed1) { RunSeed(1); }
TEST(LifecycleFuzz, Seed2) { RunSeed(2); }
TEST(LifecycleFuzz, Seed3) { RunSeed(3); }
TEST(LifecycleFuzz, Seed4) { RunSeed(4); }
TEST(LifecycleFuzz, Seed5) { RunSeed(5); }

}  // namespace
}  // namespace einheit::cli::confd
