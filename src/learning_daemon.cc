/// @file learning_daemon.cc
/// @brief LearningDaemon implementation — stateful REP + PUB with
/// a minimal candidate/commit model.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/learning_daemon.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zmq.hpp>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/protocol/msgpack_codec.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::learning {
namespace {

// Encode a key=value map as a newline-separated body. Adapters can
// decode it however they like; the shipped example adapter just
// prints it. Good enough for pedagogy.
auto EncodeKv(const std::unordered_map<std::string, std::string> &m)
    -> std::vector<std::uint8_t> {
  std::string out;
  for (const auto &[k, v] : m) {
    out += std::format("{}={}\n", k, v);
  }
  return std::vector<std::uint8_t>(out.begin(), out.end());
}

auto EncodeString(const std::string &s)
    -> std::vector<std::uint8_t> {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

auto JoinArgs(const std::vector<std::string> &args) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) out += ' ';
    out += args[i];
  }
  return out;
}

auto StatusLabel(protocol::ResponseStatus s) -> const char * {
  return s == protocol::ResponseStatus::Ok ? "ok" : "error";
}

}  // namespace

struct LearningDaemon::Impl {
  std::string ctl_ep;
  std::string pub_ep;

  zmq::context_t ctx{1};
  zmq::socket_t rep{ctx, zmq::socket_type::rep};
  zmq::socket_t pub{ctx, zmq::socket_type::pub};
  std::mutex pub_mu;

  std::mutex mu;
  std::unordered_map<std::string, std::string> running;
  std::optional<std::string> active_session;
  CandidateState candidate;
  std::vector<std::unordered_map<std::string, std::string>>
      commits;

  std::thread thread;
  std::thread heartbeat_thread;
  std::atomic<bool> stop{false};

  std::ostream *trace = nullptr;
  std::mutex trace_mu;

  std::shared_ptr<const schema::Schema> schema;
};

namespace {

auto HandleRequest(LearningDaemon::Impl &d,
                   const protocol::Request &req)
    -> protocol::Response {
  std::lock_guard<std::mutex> lk(d.mu);
  protocol::Response r;
  r.id = req.id;
  r.status = protocol::ResponseStatus::Ok;

  if (req.command == "configure") {
    d.active_session = "learn-1";
    d.candidate.values = d.running;  // seed from running
    r.data = EncodeString(*d.active_session);
    return r;
  }

  if (req.command == "set") {
    if (!d.active_session ||
        req.session_id.value_or("") != *d.active_session) {
      r.status = protocol::ResponseStatus::Error;
      r.error = protocol::ResponseError{"no_session",
                                        "run `configure` first", ""};
      return r;
    }
    if (req.args.size() < 2) {
      r.status = protocol::ResponseStatus::Error;
      r.error = protocol::ResponseError{
          "bad_args", "usage: set <path> <value>", ""};
      return r;
    }
    if (d.schema) {
      auto v = schema::ValidatePath(*d.schema, req.args[0],
                                    req.args[1]);
      if (!v) {
        r.status = protocol::ResponseStatus::Error;
        r.error = protocol::ResponseError{
            "validation", v.error().message, ""};
        return r;
      }
    }
    d.candidate.values[req.args[0]] = req.args[1];
    return r;
  }

  if (req.command == "delete") {
    if (!d.active_session) {
      r.status = protocol::ResponseStatus::Error;
      r.error = protocol::ResponseError{"no_session",
                                        "run `configure` first", ""};
      return r;
    }
    if (!req.args.empty()) d.candidate.values.erase(req.args[0]);
    return r;
  }

  if (req.command == "commit") {
    if (!d.active_session) {
      r.status = protocol::ResponseStatus::Error;
      r.error = protocol::ResponseError{
          "no_session", "nothing to commit — run `configure`", ""};
      return r;
    }
    d.running = d.candidate.values;
    d.commits.push_back(d.candidate.values);
    d.active_session.reset();
    d.candidate.values.clear();
    r.data = EncodeString(std::format("commit_id={}",
                                       d.commits.size()));
    return r;
  }

  if (req.command == "rollback") {
    if (!req.args.empty() && req.args[0] == "candidate") {
      d.active_session.reset();
      d.candidate.values.clear();
      return r;
    }
    if (!req.args.empty() && req.args[0] == "previous" &&
        d.commits.size() >= 2) {
      d.running = d.commits[d.commits.size() - 2];
      d.commits.push_back(d.running);
      return r;
    }
  }

  if (req.command == "show_config") {
    if (req.args.empty()) {
      r.data = EncodeKv(d.running);
      return r;
    }
    // Filter by prefix. `show config interfaces` matches every path
    // starting with "interfaces" plus the exact leaf match.
    const auto &prefix = req.args[0];
    std::unordered_map<std::string, std::string> filtered;
    for (const auto &[k, v] : d.running) {
      if (k == prefix || k.rfind(prefix + ".", 0) == 0) {
        filtered.emplace(k, v);
      }
    }
    r.data = EncodeKv(filtered);
    return r;
  }

  if (req.command == "show_commits") {
    std::string body;
    for (std::size_t i = 0; i < d.commits.size(); ++i) {
      body += std::format("commit_id={}\n", i + 1);
    }
    r.data = EncodeString(body);
    return r;
  }

  if (req.command == "show_commit") {
    if (req.args.empty()) {
      r.status = protocol::ResponseStatus::Error;
      r.error = protocol::ResponseError{
          "bad_args", "usage: show commit <id>", ""};
      return r;
    }
    std::size_t id = 0;
    try {
      id = static_cast<std::size_t>(std::stoul(req.args[0]));
    } catch (...) {
      r.status = protocol::ResponseStatus::Error;
      r.error = protocol::ResponseError{
          "bad_args", "commit id must be an integer", ""};
      return r;
    }
    if (id == 0 || id > d.commits.size()) {
      r.status = protocol::ResponseStatus::Error;
      r.error = protocol::ResponseError{
          "not_found",
          std::format("no such commit: {}", id), ""};
      return r;
    }
    // Diff against the previous commit: `+key=val` added,
    // `~key=val (was X)` changed, `=key=val` unchanged, `-key=val`
    // removed. Adapter colours these lines green/yellow/red.
    const auto &cur = d.commits[id - 1];
    std::unordered_map<std::string, std::string> prev;
    if (id >= 2) prev = d.commits[id - 2];
    std::string body = std::format("commit_id={}\n", id);
    for (const auto &[k, v] : cur) {
      auto it = prev.find(k);
      if (it == prev.end()) {
        body += std::format("+{}={}\n", k, v);
      } else if (it->second != v) {
        body += std::format("~{}={} (was {})\n", k, v, it->second);
      } else {
        body += std::format("={}={}\n", k, v);
      }
    }
    for (const auto &[k, v] : prev) {
      if (!cur.contains(k)) {
        body += std::format("-{}={}\n", k, v);
      }
    }
    r.data = EncodeString(body);
    return r;
  }

  if (req.command == "show_status") {
    const auto txt = std::format(
        "learning mode\ncommits={}\nsession={}\ncandidate_keys={}\n",
        d.commits.size(),
        d.active_session.value_or("<none>"),
        d.candidate.values.size());
    r.data = EncodeString(txt);
    return r;
  }

  if (req.command == "shell_enter" || req.command == "shell_exit") {
    // Accept audit bookends silently.
    return r;
  }

  r.status = protocol::ResponseStatus::Error;
  r.error = protocol::ResponseError{
      "unknown",
      std::format("learning daemon: unknown command '{}'",
                  req.command),
      ""};
  return r;
}

auto Loop(LearningDaemon::Impl &d) -> void {
  zmq::pollitem_t item{static_cast<void *>(d.rep), 0, ZMQ_POLLIN, 0};
  while (!d.stop.load()) {
    if (zmq::poll(&item, 1, std::chrono::milliseconds(50)) <= 0) {
      continue;
    }
    zmq::message_t msg;
    auto got = d.rep.recv(msg, zmq::recv_flags::none);
    if (!got) continue;

    const auto *p =
        reinterpret_cast<const std::uint8_t *>(msg.data());
    auto req = protocol::DecodeRequest(
        std::span<const std::uint8_t>(p, msg.size()));
    protocol::Response resp;
    if (!req) {
      resp.status = protocol::ResponseStatus::Error;
      resp.error =
          protocol::ResponseError{"decode", req.error().message, ""};
    } else {
      resp = HandleRequest(d, *req);
    }
    if (d.trace) {
      std::lock_guard<std::mutex> lk(d.trace_mu);
      if (req) {
        *d.trace << std::format(
            "[learn] → {} {}\n", req->command,
            JoinArgs(req->args));
      }
      *d.trace << std::format("[learn] ← {}", StatusLabel(resp.status));
      if (resp.error) {
        *d.trace << std::format(" ({}: {})", resp.error->code,
                                resp.error->message);
      } else if (!resp.data.empty()) {
        *d.trace << std::format(" [{} bytes]", resp.data.size());
      }
      *d.trace << '\n';
    }
    auto bytes = protocol::EncodeResponse(resp);
    if (!bytes) continue;
    zmq::message_t out(bytes->data(), bytes->size());
    d.rep.send(out, zmq::send_flags::none);
  }
}

}  // namespace

LearningDaemon::LearningDaemon() : LearningDaemon(nullptr) {}

LearningDaemon::LearningDaemon(std::ostream *trace)
    : LearningDaemon(trace, nullptr) {}

LearningDaemon::LearningDaemon(
    std::ostream *trace,
    std::shared_ptr<const schema::Schema> schema)
    : impl_(std::make_unique<Impl>()) {
  impl_->trace = trace;
  impl_->schema = std::move(schema);
  const auto base =
      std::filesystem::temp_directory_path() /
      std::format("einheit_learn_{}_{}",
                  static_cast<int>(::getpid()),
                  reinterpret_cast<std::uintptr_t>(this));
  impl_->ctl_ep = std::format("ipc://{}.ctl", base.string());
  impl_->pub_ep = std::format("ipc://{}.pub", base.string());

  impl_->rep.set(zmq::sockopt::linger, 0);
  impl_->pub.set(zmq::sockopt::linger, 0);
  impl_->rep.bind(impl_->ctl_ep);
  impl_->pub.bind(impl_->pub_ep);

  impl_->thread = std::thread([d = impl_.get()]() { Loop(*d); });

  // Heartbeat publisher: emits mock metric events on
  // state.metrics.* every ~300ms so `watch metrics` has something
  // to draw. Random int in data field; adapter parses + folds into
  // a rolling sparkline.
  impl_->heartbeat_thread = std::thread([d = impl_.get()]() {
    std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(5, 95);
    while (!d->stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      if (d->stop.load()) break;
      for (const char *name : {"latency_ms", "tx_pps"}) {
        protocol::Event ev;
        ev.topic = std::format("state.metrics.{}", name);
        ev.timestamp = "";
        const auto value = std::to_string(dist(rng));
        ev.data.assign(value.begin(), value.end());
        auto body = protocol::EncodeEventBody(ev);
        if (!body) continue;
        std::lock_guard<std::mutex> lk(d->pub_mu);
        try {
          zmq::message_t topic_frame(ev.topic.data(),
                                      ev.topic.size());
          d->pub.send(topic_frame, zmq::send_flags::sndmore);
          zmq::message_t body_frame(body->data(), body->size());
          d->pub.send(body_frame, zmq::send_flags::none);
        } catch (...) {
          // shutting down
        }
      }
    }
  });
}

LearningDaemon::~LearningDaemon() {
  impl_->stop.store(true);
  if (impl_->thread.joinable()) impl_->thread.join();
  if (impl_->heartbeat_thread.joinable()) {
    impl_->heartbeat_thread.join();
  }
}

auto LearningDaemon::ControlEndpoint() const -> const std::string & {
  return impl_->ctl_ep;
}

auto LearningDaemon::EventEndpoint() const -> const std::string & {
  return impl_->pub_ep;
}

}  // namespace einheit::cli::learning
