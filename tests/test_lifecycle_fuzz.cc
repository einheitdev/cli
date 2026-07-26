/// @file test_lifecycle_fuzz.cc
/// @brief Model-based fuzzing of the confd lifecycle state machine.
///
/// Random-but-deterministic sequences of configure / set / delete /
/// commit / rollback (candidate, previous, to <id>) / process
/// restart / reboot-with-boot-apply run against a real Runtime +
/// MemoryBackend and are checked op-by-op against a ~60-line
/// reference model of what running config and history must be. This
/// is the layer that catches state-machine-over-time bugs (the
/// stale-persisted-running reconcile bug was exactly this class):
/// example-based tests never think to write the sequence that
/// breaks. commit-confirmed is excluded — its timer semantics don't
/// fuzz on a unit-test clock and have their own suite.
///
/// A second generator, LockFuzz, drives TWO Runtimes over one state
/// directory to fuzz the edit-lock protocol: that is the shape a
/// product which embeds confd in its CLI binary actually deploys, so
/// "who may edit, who may commit" is a cross-process question there.
/// Its model tracks the box and the lock file instead of each
/// runtime's private view, because a runtime only adopts another's
/// commits when it opens a session.
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

#include "einheit/cli/confd/edit_lock.h"
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
         const std::string &session, const std::string &user = "fuzz")
    -> protocol::Request {
  protocol::Request r;
  r.id = "fuzz";
  r.user = user;
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
    } else if (roll < 95) {
      // Process restart: durable state must survive, the open
      // session must not, and startup reconciliation must leave
      // running matching the box exactly.
      what = "restart";
      rt.reset();
      rt.emplace(backend, opts);
      model.session_open = false;
      session.clear();
    } else {
      // Reboot: the process goes away AND the box comes up blank.
      // Boot-apply has to put the committed configuration back, which
      // is the difference between a switch that survives a power cut
      // and one that wakes up factory-fresh.
      what = "reboot";
      rt.reset();
      backend.ResetDevice();
      rt.emplace(backend, opts);
      model.session_open = false;
      session.clear();
      auto restored = rt->ApplyRunningAtBoot();
      ASSERT_TRUE(restored.has_value()) << std::format(
          "op {} ({}): seed {}: boot apply failed: {}", op, what, seed,
          restored ? "" : restored.error().message);
      ASSERT_EQ(restored->applied, !model.history.empty())
          << std::format("op {} ({}): seed {}", op, what, seed);
      const auto box = DiffConfigs(model.running, backend.DeviceState());
      ASSERT_TRUE(box.empty()) << std::format(
          "op {} ({}): seed {}: box not restored: {}", op, what, seed,
          box);
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

/// Reference model for the two-session mix. Deliberately NOT a model
/// of each runtime's private running config: a runtime only adopts
/// another's commits when it opens a session, so the shared truths are
/// the box and the lock file, and those are what this asserts.
struct LockModel {
  /// Per-client candidate session id, empty when the client has none.
  /// A client keeps its session (and can still edit its candidate)
  /// after being force-displaced — it just cannot commit.
  std::string session[2];
  /// Per-client candidate contents.
  Config candidate[2];
  /// Session id in the lock file, empty when configure mode is free.
  std::string lock;
  /// What the box holds.
  Config running;
};

auto RunLockSeed(std::uint32_t seed) -> void {
  const auto state_dir =
      fs::temp_directory_path() /
      std::format("confd-lockfuzz-{}-{}", ::getpid(), seed);
  fs::remove_all(state_dir);

  auto schema = schema::LoadSchemaFromString(kSchemaYaml);
  ASSERT_TRUE(schema.has_value());
  MemoryBackend backend(*schema);
  RuntimeOptions opts;
  opts.state_dir = state_dir.string();

  // Two clients, two Runtimes, one store — the embedded deployment.
  std::optional<Runtime> rt[2];
  rt[0].emplace(backend, opts);
  rt[1].emplace(backend, opts);
  const char *user[2] = {"alice", "bob"};

  LockModel model;
  std::mt19937 rng(seed);
  const auto pick = [&rng](std::size_t n) -> std::size_t {
    return rng() % n;
  };

  // The lock file is the cross-process truth, so read it directly
  // rather than through either runtime's view of itself.
  const auto lock_session = [&]() -> std::string {
    auto held = ReadEditLock(opts.state_dir);
    if (!held || !held->has_value()) return {};
    return (*held)->session_id;
  };

  for (int op = 0; op < 200; ++op) {
    const auto c = pick(2);
    const auto roll = rng() % 100;
    std::string what;

    if (roll < 16) {
      // configure — refused if this client already has a session, and
      // refused if the other client holds the lock.
      what = std::format("{} configure", user[c]);
      const bool expect_ok = model.session[c].empty() && model.lock.empty();
      const auto resp =
          rt[c]->HandleRequest(Req("configure", {}, "", user[c]));
      ASSERT_EQ(Ok(resp), expect_ok) << std::format(
          "op {} {}: seed {} ({})", op, what, seed,
          resp.error ? resp.error->message : "ok");
      if (Ok(resp)) {
        model.session[c] = Body(resp);
        model.lock = model.session[c];
        model.candidate[c] = model.running;
      } else {
        ASSERT_EQ(resp.error->code, "session_busy") << std::format(
            "op {} {}: seed {}", op, what, seed);
      }
    } else if (roll < 24) {
      // configure force — always granted, always takes the lock. The
      // displaced client keeps its (now uncommittable) candidate.
      what = std::format("{} configure force", user[c]);
      const auto resp =
          rt[c]->HandleRequest(Req("configure_force", {}, "", user[c]));
      ASSERT_TRUE(Ok(resp)) << std::format(
          "op {} {}: seed {} ({})", op, what, seed,
          resp.error ? resp.error->message : "ok");
      model.session[c] = Body(resp);
      model.lock = model.session[c];
      model.candidate[c] = model.running;
    } else if (roll < 50) {
      // set — a candidate edit, so it needs only this client's own
      // session, not the lock.
      const auto &pool = kPools[pick(kPools.size())];
      const bool try_invalid =
          !pool.invalid.empty() && (rng() % 10 == 0);
      const auto &value =
          try_invalid ? pool.invalid[pick(pool.invalid.size())]
                      : pool.valid[pick(pool.valid.size())];
      what = std::format("{} set {} {}", user[c], pool.path, value);
      const auto resp = rt[c]->HandleRequest(
          Req("set", {pool.path, value}, model.session[c], user[c]));
      const bool expect_ok = !model.session[c].empty() && !try_invalid;
      ASSERT_EQ(Ok(resp), expect_ok) << std::format(
          "op {} {}: seed {} ({})", op, what, seed,
          resp.error ? resp.error->message : "ok");
      if (Ok(resp)) model.candidate[c][pool.path] = value;
    } else if (roll < 56) {
      what = std::format("{} delete", user[c]);
      const auto &pool = kPools[pick(kPools.size())];
      const auto resp = rt[c]->HandleRequest(
          Req("delete", {pool.path}, model.session[c], user[c]));
      ASSERT_EQ(Ok(resp), !model.session[c].empty()) << std::format(
          "op {} {}: seed {}", op, what, seed);
      if (Ok(resp)) model.candidate[c].erase(pool.path);
    } else if (roll < 80) {
      // commit — needs a session AND the lock. The lock half is what
      // stops a displaced editor from clobbering the new holder.
      what = std::format("{} commit", user[c]);
      const bool has_session = !model.session[c].empty();
      const bool holds_lock = has_session && model.lock == model.session[c];
      const auto resp =
          rt[c]->HandleRequest(Req("commit", {}, model.session[c], user[c]));
      ASSERT_EQ(Ok(resp), holds_lock) << std::format(
          "op {} {}: seed {} ({})", op, what, seed,
          resp.error ? resp.error->message : "ok");
      if (Ok(resp)) {
        model.running = model.candidate[c];
        model.session[c].clear();
        model.lock.clear();
      } else {
        ASSERT_EQ(resp.error->code,
                  has_session ? "lock_lost" : "no_session")
            << std::format("op {} {}: seed {}", op, what, seed);
      }
    } else if (roll < 88) {
      // rollback candidate — drops this client's session and, with it,
      // the lock if this client was holding it. The path `exit` takes.
      what = std::format("{} rollback candidate", user[c]);
      const auto resp = rt[c]->HandleRequest(
          Req("rollback", {}, model.session[c], user[c]));
      ASSERT_TRUE(Ok(resp)) << std::format("op {} {}: seed {}", op, what,
                                           seed);
      if (model.lock == model.session[c]) model.lock.clear();
      model.session[c].clear();
    } else if (roll < 94) {
      // Restart one client's process: its session dies and its lock
      // goes with it, while the other client is untouched.
      what = std::format("{} restart", user[c]);
      if (model.lock == model.session[c] && !model.session[c].empty()) {
        model.lock.clear();
      }
      model.session[c].clear();
      rt[c].reset();
      rt[c].emplace(backend, opts);
    } else {
      // Reboot one client: process gone, box blank, boot-apply must put
      // the committed configuration back. The other client's runtime
      // stays up, which is the awkward case worth fuzzing.
      what = std::format("{} reboot", user[c]);
      if (model.lock == model.session[c] && !model.session[c].empty()) {
        model.lock.clear();
      }
      model.session[c].clear();
      rt[c].reset();
      backend.ResetDevice();
      rt[c].emplace(backend, opts);
      auto restored = rt[c]->ApplyRunningAtBoot();
      ASSERT_TRUE(restored.has_value()) << std::format(
          "op {} {}: seed {}", op, what, seed);
    }

    // The box is the shared ground truth: whatever the two sessions
    // did, it must hold exactly the last committed configuration.
    const auto box = DiffConfigs(model.running, backend.DeviceState());
    ASSERT_TRUE(box.empty()) << std::format(
        "op {} ({}): seed {}: box diverged: {}", op, what, seed, box);
    ASSERT_EQ(lock_session(), model.lock) << std::format(
        "op {} ({}): seed {}: lock holder diverged", op, what, seed);
    // Exactly one editor at a time, by construction of the lock.
    const bool a_holds = !model.lock.empty() && model.lock == model.session[0];
    const bool b_holds = !model.lock.empty() && model.lock == model.session[1];
    ASSERT_FALSE(a_holds && b_holds) << std::format(
        "op {} ({}): seed {}: two holders", op, what, seed);
  }

  rt[0].reset();
  rt[1].reset();
  fs::remove_all(state_dir);
}

TEST(LifecycleFuzz, Seed1) { RunSeed(1); }
TEST(LifecycleFuzz, Seed2) { RunSeed(2); }
TEST(LifecycleFuzz, Seed3) { RunSeed(3); }
TEST(LifecycleFuzz, Seed4) { RunSeed(4); }
TEST(LifecycleFuzz, Seed5) { RunSeed(5); }

TEST(LockFuzz, Seed1) { RunLockSeed(1); }
TEST(LockFuzz, Seed2) { RunLockSeed(2); }
TEST(LockFuzz, Seed3) { RunLockSeed(3); }
TEST(LockFuzz, Seed4) { RunLockSeed(4); }
TEST(LockFuzz, Seed5) { RunLockSeed(5); }

}  // namespace
}  // namespace einheit::cli::confd
