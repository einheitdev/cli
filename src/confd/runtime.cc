/// @file runtime.cc
/// @brief confd daemon runtime implementation — candidate/commit/
/// rollback lifecycle over a ConfigBackend.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "einheit/cli/audit.h"
#include "einheit/cli/confd/store.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {
namespace {

// Current wall-clock time as epoch milliseconds (UTC). Confirm
// deadlines are absolute so they survive a restart.
auto NowMs() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Parse a confirm window in minutes; fractional values are accepted so
// operators can pick sub-minute windows (e.g. 0.5 = 30s) and tests can
// use short deadlines. Returns nullopt on non-numeric / non-positive.
auto ParseMinutes(const std::string &s) -> std::optional<double> {
  if (s.empty()) return std::nullopt;
  char *end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0') return std::nullopt;
  if (!(v > 0.0)) return std::nullopt;
  return v;
}

// Encode a flat config map as newline-separated key=value lines — the
// same body shape learning_daemon used, so existing adapters render it
// unchanged.
auto EncodeKv(const Config &m) -> std::vector<std::uint8_t> {
  std::string out;
  for (const auto &[k, v] : m) {
    out += std::format("{}={}\n", k, v);
  }
  return std::vector<std::uint8_t>(out.begin(), out.end());
}

auto EncodeString(const std::string &s) -> std::vector<std::uint8_t> {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

auto ErrorResponse(const protocol::Request &req, std::string code,
                   std::string message) -> protocol::Response {
  protocol::Response r;
  r.id = req.id;
  r.status = protocol::ResponseStatus::Error;
  r.error = protocol::ResponseError{std::move(code), std::move(message), ""};
  return r;
}

// Parse an unsigned commit id from a string. Returns nullopt on any
// non-numeric input — never throws (gap #5 lesson: no raw std::stoul).
auto ParseCommitId(const std::string &s) -> std::optional<CommitId> {
  if (s.empty()) return std::nullopt;
  CommitId out = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    out = out * 10 + static_cast<CommitId>(c - '0');
  }
  return out;
}

}  // namespace

// One in-flight configure session. Only one is active at a time —
// confd is the single authority, so a second editor is refused rather
// than allowed to clobber the candidate (edit-locking).
struct ActiveSession {
  std::string id;
  Candidate candidate;
  std::string author;
};

struct Runtime::Impl {
  ConfigBackend &backend;
  audit::Sink audit;
  std::string state_dir;

  mutable std::mutex mu;
  Config running;
  std::optional<ActiveSession> active;
  std::vector<CommitRecord> history;
  // Runtime-owned canonical revision counter; durable across restarts
  // (and backend restarts) so ids never collide or repeat.
  CommitId next_rev = 0;
  PendingConfirm pending;
  std::uint64_t session_counter = 0;

  // Auto-revert timer. The thread lives in the daemon process, which
  // outlives any CLI / SSH session — this is what makes commit-confirmed
  // safe: severing the client cannot stop the revert. `cv` wakes the
  // thread when the pending window is armed, disarmed, or re-armed.
  std::condition_variable cv;
  std::thread timer;
  std::atomic<bool> stop{false};

  explicit Impl(ConfigBackend &b) : backend(b) {}

  // Write the full durable state. Best-effort: a write failure is
  // surfaced to the audit sink but does not abort the in-memory op.
  auto Persist(const protocol::Request &req) -> void {
    if (state_dir.empty()) return;
    PersistentState st;
    st.running = running;
    st.history = history;
    st.next_rev = next_rev;
    st.pending = pending;
    if (auto r = SaveState(state_dir, st); !r) {
      Emit(req, "persist", false, r.error().message);
    }
  }

  auto Emit(const protocol::Request &req, const std::string &command, bool ok,
            const std::string &outcome) -> void {
    if (!audit) return;
    audit::Record rec;
    rec.timestamp = audit::NowTimestamp();
    rec.user = req.user;
    rec.role = req.role;
    rec.command = command;
    rec.wire_command = req.command;
    rec.args = req.args;
    if (active) rec.session_id = active->id;
    rec.ok = ok;
    rec.outcome = outcome;
    audit(rec);
  }
};

namespace {

auto HandleConfigure(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  protocol::Response r;
  r.id = req.id;
  if (d.active) {
    d.Emit(req, "configure", false, "session busy");
    return ErrorResponse(req, "session_busy",
                         std::format("a configure session is already open ({})",
                                     d.active->author));
  }
  ActiveSession s;
  s.id = std::format("confd-{}", ++d.session_counter);
  s.candidate.values = d.running;  // seed from running
  s.author = req.user;
  d.active = std::move(s);
  r.data = EncodeString(d.active->id);
  d.Emit(req, "configure", true, "ok");
  return r;
}

auto HandleSet(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  if (!d.active || req.session_id.value_or("") != d.active->id) {
    d.Emit(req, "set", false, "no session");
    return ErrorResponse(req, "no_session", "run `configure` first");
  }
  if (req.args.size() < 2) {
    d.Emit(req, "set", false, "bad args");
    return ErrorResponse(req, "bad_args", "usage: set <path> <value>");
  }
  // Validate against the schema when the product actually defines one;
  // a backend with an empty schema (bare product) accepts free-form
  // paths, matching the daemon's pre-schema behaviour.
  const auto &schema = d.backend.Schema();
  if (!schema.root.fields.empty()) {
    auto v = schema::ValidatePath(schema, req.args[0], req.args[1]);
    if (!v) {
      d.Emit(req, "set", false, "validation");
      return ErrorResponse(req, "validation", v.error().message);
    }
  }
  d.active->candidate.values[req.args[0]] = req.args[1];
  d.Emit(req, "set", true, "ok");
  protocol::Response r;
  r.id = req.id;
  return r;
}

auto HandleDelete(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  if (!d.active || req.session_id.value_or("") != d.active->id) {
    d.Emit(req, "delete", false, "no session");
    return ErrorResponse(req, "no_session", "run `configure` first");
  }
  if (!req.args.empty()) {
    d.active->candidate.values.erase(req.args[0]);
  }
  d.Emit(req, "delete", true, "ok");
  protocol::Response r;
  r.id = req.id;
  return r;
}

// Apply a candidate through the backend and, on success, record a
// commit and advance running state. The single place config reaches
// hardware — gap #4's fix in force. The runtime owns the canonical
// revision id; the backend's returned handle is stored alongside.
auto ApplyAndRecord(Runtime::Impl &d, const protocol::Request &req,
                    const Candidate &candidate, const std::string &author)
    -> std::expected<CommitId, Error<ApplyError>> {
  auto applied = d.backend.Apply(candidate);
  if (!applied) return applied;
  CommitRecord c;
  c.id = ++d.next_rev;
  c.backend_id = *applied;
  c.candidate = candidate;
  c.author = author;
  c.timestamp = audit::NowTimestamp();
  const CommitId new_id = c.id;
  // `candidate` may be a reference *into* d.history — rollback
  // re-applies a historical commit's stored candidate. push_back can
  // reallocate d.history, freeing the storage `candidate` points at,
  // so we must not read `candidate` afterwards. Read running state
  // from our own copy (c.candidate) and capture the id before the
  // move. (ASan heap-use-after-free, gap: memory-safety invariants.)
  d.running = c.candidate.values;
  d.history.push_back(std::move(c));
  d.Persist(req);
  return new_id;
}

// Fire the auto-revert: re-apply the pre-confirm configuration and
// clear the pending window. Caller holds mu. This runs from the timer
// thread (which lives in the daemon process, outliving the client
// session) or from restart recovery — never from the client.
auto AutoRevert(Runtime::Impl &d) -> void {
  Candidate target;  // empty target => revert to empty config
  if (d.pending.rollback_to != 0) {
    for (const auto &c : d.history) {
      if (c.id == d.pending.rollback_to) {
        target = c.candidate;
        break;
      }
    }
  }
  protocol::Request note;
  note.command = "auto_revert";
  note.user = "confd";
  const auto reverted_to = d.pending.rollback_to;
  auto applied = d.backend.Apply(target);
  if (applied) {
    CommitRecord c;
    c.id = ++d.next_rev;
    c.backend_id = *applied;
    c.candidate = target;
    c.author = "confd-auto-revert";
    c.timestamp = audit::NowTimestamp();
    d.history.push_back(std::move(c));
    d.running = target.values;
  }
  // Disarm regardless: a backend that cannot revert is a serious fault,
  // but re-firing in a tight loop would only make it worse.
  d.pending = PendingConfirm{};
  d.Persist(note);
  d.Emit(note, "auto-revert", applied.has_value(),
         applied ? std::format("commit-confirm expired; reverted to commit {}",
                               reverted_to)
                 : applied.error().message);
}

// Background loop: wait until the armed deadline, then auto-revert.
// Woken early whenever the window is armed, disarmed, or re-armed.
auto TimerLoop(Runtime::Impl &d) -> void {
  std::unique_lock<std::mutex> lk(d.mu);
  while (!d.stop.load()) {
    if (!d.pending.armed) {
      d.cv.wait(lk, [&] { return d.stop.load() || d.pending.armed; });
      continue;
    }
    const auto now = NowMs();
    if (now >= d.pending.deadline_epoch_ms) {
      AutoRevert(d);
      continue;
    }
    d.cv.wait_for(lk,
                  std::chrono::milliseconds(d.pending.deadline_epoch_ms - now));
    // Re-evaluate from the top: the deadline may have moved, the window
    // may have been confirmed, or we may be shutting down.
  }
}

auto HandleCommit(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  if (!d.active) {
    d.Emit(req, "commit", false, "no session");
    return ErrorResponse(req, "no_session",
                         "nothing to commit — run `configure`");
  }
  auto applied = ApplyAndRecord(d, req, d.active->candidate, d.active->author);
  if (!applied) {
    // Keep the session so the operator can fix and retry; running
    // state is untouched.
    d.Emit(req, "commit", false, "apply failed");
    return ErrorResponse(req, "apply_failed", applied.error().message);
  }
  d.active.reset();
  // A plain commit while a confirm window is open confirms/supersedes
  // it: the fresh commit is now running, so there is nothing to revert.
  bool superseded = false;
  if (d.pending.armed) {
    d.pending = PendingConfirm{};
    superseded = true;
    d.Persist(req);
    d.cv.notify_all();
  }
  d.Emit(req, "commit", true, superseded ? "ok (confirmed pending)" : "ok");
  protocol::Response r;
  r.id = req.id;
  r.data = EncodeString(std::format("commit_id={}", *applied));
  return r;
}

auto HandleCommitConfirmed(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  if (!d.active) {
    d.Emit(req, "commit confirmed", false, "no session");
    return ErrorResponse(req, "no_session",
                         "nothing to commit — run `configure`");
  }
  if (req.args.empty()) {
    d.Emit(req, "commit confirmed", false, "bad args");
    return ErrorResponse(req, "bad_args", "usage: commit confirmed <minutes>");
  }
  auto minutes = ParseMinutes(req.args[0]);
  if (!minutes) {
    d.Emit(req, "commit confirmed", false, "bad minutes");
    return ErrorResponse(
        req, "bad_args",
        "commit confirmed <minutes>: minutes must be positive");
  }
  auto applied = ApplyAndRecord(d, req, d.active->candidate, d.active->author);
  if (!applied) {
    d.Emit(req, "commit confirmed", false, "apply failed");
    return ErrorResponse(req, "apply_failed", applied.error().message);
  }
  d.active.reset();
  // Arm the auto-revert. Revert target is the commit BEFORE the one we
  // just applied (0 => empty config, when this is the first commit).
  d.pending.armed = true;
  d.pending.pending_commit = *applied;
  d.pending.rollback_to =
      d.history.size() >= 2 ? d.history[d.history.size() - 2].id : 0;
  d.pending.deadline_epoch_ms =
      NowMs() + static_cast<std::int64_t>(*minutes * 60000.0);
  d.Persist(req);
  d.cv.notify_all();  // wake the timer to arm the window
  d.Emit(req, "commit confirmed", true,
         std::format("auto-revert armed for {} min", *minutes));
  protocol::Response r;
  r.id = req.id;
  const auto secs = (d.pending.deadline_epoch_ms - NowMs()) / 1000;
  r.data = EncodeString(
      std::format("commit_id={} confirm_within_s={}", *applied, secs));
  return r;
}

auto HandleConfirm(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  if (!d.pending.armed) {
    d.Emit(req, "confirm", false, "nothing pending");
    return ErrorResponse(req, "not_pending", "no commit-confirm is pending");
  }
  const auto confirmed = d.pending.pending_commit;
  d.pending = PendingConfirm{};
  d.Persist(req);
  d.cv.notify_all();  // stand the timer down
  d.Emit(req, "confirm", true, std::format("confirmed commit {}", confirmed));
  protocol::Response r;
  r.id = req.id;
  r.data = EncodeString(std::format("confirmed commit_id={}", confirmed));
  return r;
}

// Re-apply a historical commit's candidate as a new commit. Used by
// rollback previous / rollback to <id>.
auto RollbackTo(Runtime::Impl &d, const protocol::Request &req,
                const CommitRecord &target, const std::string &label)
    -> protocol::Response {
  auto applied = ApplyAndRecord(d, req, target.candidate, req.user);
  if (!applied) {
    d.Emit(req, label, false, "apply failed");
    return ErrorResponse(req, "apply_failed", applied.error().message);
  }
  d.Emit(req, label, true, "ok");
  protocol::Response r;
  r.id = req.id;
  r.data = EncodeString(std::format("commit_id={}", *applied));
  return r;
}

auto HandleRollback(Runtime::Impl &d, const protocol::Request &req,
                    const std::string &mode) -> protocol::Response {
  if (mode == "candidate") {
    d.active.reset();
    d.Emit(req, "rollback candidate", true, "ok");
    protocol::Response r;
    r.id = req.id;
    return r;
  }
  if (mode == "previous") {
    if (d.history.size() < 2) {
      d.Emit(req, "rollback previous", false, "no previous commit");
      return ErrorResponse(req, "not_found",
                           "no previous commit to roll back to");
    }
    return RollbackTo(d, req, d.history[d.history.size() - 2],
                      "rollback previous");
  }
  // Numeric revision id.
  if (auto id = ParseCommitId(mode)) {
    for (const auto &c : d.history) {
      if (c.id == *id) {
        return RollbackTo(d, req, c, std::format("rollback to {}", *id));
      }
    }
    d.Emit(req, "rollback", false, "no such commit");
    return ErrorResponse(req, "not_found",
                         std::format("no such commit: {}", mode));
  }
  d.Emit(req, "rollback", false, "bad target");
  return ErrorResponse(req, "bad_args",
                       "usage: rollback candidate | previous | <commit-id>");
}

auto HandleShowConfig(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  protocol::Response r;
  r.id = req.id;
  if (req.args.empty()) {
    r.data = EncodeKv(d.running);
    return r;
  }
  const auto &prefix = req.args[0];
  Config filtered;
  for (const auto &[k, v] : d.running) {
    if (k == prefix || k.rfind(prefix + ".", 0) == 0) {
      filtered.emplace(k, v);
    }
  }
  r.data = EncodeKv(filtered);
  return r;
}

auto HandleShowCommits(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  protocol::Response r;
  r.id = req.id;
  std::string body;
  for (const auto &c : d.history) {
    body +=
        std::format("commit_id={} by={} at={}\n", c.id, c.author, c.timestamp);
  }
  r.data = EncodeString(body);
  return r;
}

auto HandleShowCommit(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  if (req.args.empty()) {
    return ErrorResponse(req, "bad_args", "usage: show commit <id>");
  }
  auto id = ParseCommitId(req.args[0]);
  if (!id) {
    return ErrorResponse(req, "bad_args", "commit id must be an integer");
  }
  const CommitRecord *cur = nullptr;
  const CommitRecord *prev = nullptr;
  for (std::size_t i = 0; i < d.history.size(); ++i) {
    if (d.history[i].id == *id) {
      cur = &d.history[i];
      if (i > 0) prev = &d.history[i - 1];
      break;
    }
  }
  if (!cur) {
    return ErrorResponse(req, "not_found",
                         std::format("no such commit: {}", *id));
  }
  // Diff against the previous commit: +added ~changed =unchanged
  // -removed. Adapters colour these markers.
  Config prev_values;
  if (prev) prev_values = prev->candidate.values;
  std::string body = std::format("commit_id={}\n", *id);
  for (const auto &[k, v] : cur->candidate.values) {
    auto it = prev_values.find(k);
    if (it == prev_values.end()) {
      body += std::format("+{}={}\n", k, v);
    } else if (it->second != v) {
      body += std::format("~{}={} (was {})\n", k, v, it->second);
    } else {
      body += std::format("={}={}\n", k, v);
    }
  }
  for (const auto &[k, v] : prev_values) {
    if (!cur->candidate.values.contains(k)) {
      body += std::format("-{}={}\n", k, v);
    }
  }
  protocol::Response r;
  r.id = req.id;
  r.data = EncodeString(body);
  return r;
}

auto HandleShowStatus(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  protocol::Response r;
  r.id = req.id;
  std::string txt = std::format(
      "confd\ncommits={}\nsession={}\nrunning_keys={}\n", d.history.size(),
      d.active ? d.active->id : std::string("<none>"), d.running.size());
  // The countdown is queryable here so a reconnecting session can see
  // "N seconds until rollback" and decide whether to `confirm`.
  if (d.pending.armed) {
    const auto remaining_ms = d.pending.deadline_epoch_ms - NowMs();
    const auto remaining_s = remaining_ms > 0 ? remaining_ms / 1000 : 0;
    txt += std::format(
        "confirm_pending=yes\nconfirm_commit={}\n"
        "confirm_seconds_remaining={}\nconfirm_rollback_to={}\n",
        d.pending.pending_commit, remaining_s, d.pending.rollback_to);
  } else {
    txt += "confirm_pending=no\n";
  }
  r.data = EncodeString(txt);
  return r;
}

}  // namespace

Runtime::Runtime(ConfigBackend &backend, RuntimeOptions opts)
    : impl_(std::make_unique<Impl>(backend)) {
  impl_->audit = std::move(opts.audit);
  impl_->state_dir = std::move(opts.state_dir);

  // Recover durable state if a state dir is configured and holds a
  // prior run. Persisted running/history is the authority (it survives
  // a reboot that wiped the box); with none, seed from the backend.
  bool loaded_state = false;
  if (!impl_->state_dir.empty()) {
    if (auto loaded = LoadState(impl_->state_dir); loaded) {
      if (!loaded->history.empty() || !loaded->running.empty() ||
          loaded->next_rev > 0 || loaded->pending.armed) {
        impl_->running = std::move(loaded->running);
        impl_->history = std::move(loaded->history);
        impl_->next_rev = loaded->next_rev;
        impl_->pending = loaded->pending;
        loaded_state = true;
      }
    } else {
      // Corrupt state file — do not silently discard history. Fall
      // through to the backend read and surface it on the audit sink.
      protocol::Request note;
      note.command = "load";
      impl_->Emit(note, "load", false, loaded.error().message);
    }
  }
  if (!loaded_state) {
    impl_->running = backend.ReadRunning();
  }

  // Recovery: a commit-confirm window whose deadline elapsed while
  // confd was down fires immediately; a still-live one is picked up by
  // the timer thread. This is what makes the auto-revert survive a
  // confd restart, not just a severed client session.
  if (impl_->pending.armed && NowMs() >= impl_->pending.deadline_epoch_ms) {
    AutoRevert(*impl_);
  }

  impl_->timer = std::thread([d = impl_.get()]() { TimerLoop(*d); });
}

Runtime::~Runtime() {
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->stop.store(true);
  }
  impl_->cv.notify_all();
  if (impl_->timer.joinable()) impl_->timer.join();
}

auto Runtime::HandleRequest(const protocol::Request &req)
    -> protocol::Response {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto &d = *impl_;

  if (req.command == "configure") return HandleConfigure(d, req);
  if (req.command == "set") return HandleSet(d, req);
  if (req.command == "delete") return HandleDelete(d, req);
  if (req.command == "commit") return HandleCommit(d, req);
  if (req.command == "commit_confirmed") {
    return HandleCommitConfirmed(d, req);
  }
  if (req.command == "confirm") return HandleConfirm(d, req);
  if (req.command == "rollback") {
    const std::string mode =
        req.args.empty() ? std::string("candidate") : req.args[0];
    return HandleRollback(d, req, mode);
  }
  if (req.command == "rollback_previous") {
    return HandleRollback(d, req, "previous");
  }
  if (req.command == "rollback_to") {
    return HandleRollback(d, req,
                          req.args.empty() ? std::string() : req.args[0]);
  }
  if (req.command == "show_config") return HandleShowConfig(d, req);
  if (req.command == "show_commits") return HandleShowCommits(d, req);
  if (req.command == "show_commit") return HandleShowCommit(d, req);
  if (req.command == "show_status") return HandleShowStatus(d, req);

  // Handshake + audit bookends the runtime accepts silently.
  if (req.command == "describe" || req.command == "shell_enter" ||
      req.command == "shell_exit") {
    protocol::Response r;
    r.id = req.id;
    return r;
  }

  return ErrorResponse(req, "unknown",
                       std::format("confd: unknown command '{}'", req.command));
}

auto Runtime::Running() const -> Config {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->running;
}

auto Runtime::HistorySize() const -> std::size_t {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->history.size();
}

auto Runtime::PendingConfirmState() const -> PendingConfirm {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->pending;
}

}  // namespace einheit::cli::confd
