/// @file runtime.cc
/// @brief confd daemon runtime implementation — candidate/commit/
/// rollback lifecycle over a ConfigBackend.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/runtime.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "einheit/cli/audit.h"
#include "einheit/cli/confd/boot_report.h"
#include "einheit/cli/confd/config_file.h"
#include "einheit/cli/confd/edit_lock.h"
#include "einheit/cli/confd/store.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::confd {
namespace {

// Session ids must be unique among every Runtime that can reach one
// state directory, because the edit lock compares them to decide who
// holds configure mode. The pid separates processes; this counter is
// process-wide rather than per-Runtime so that two Runtimes in one
// process cannot mint the same id and be mistaken for each other.
std::atomic<std::uint64_t> g_session_counter{0};

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

// The renderer gives `message` one line and truncates the overflow, so
// anything the operator needs in order to act belongs in `hint`, which
// gets its own line — not appended to the message where it is exactly
// the part that gets cut off.
auto ErrorResponse(const protocol::Request &req, std::string code,
                   std::string message, std::string hint = "")
    -> protocol::Response {
  protocol::Response r;
  r.id = req.id;
  r.status = protocol::ResponseStatus::Error;
  r.error = protocol::ResponseError{std::move(code), std::move(message),
                                    std::move(hint)};
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
  std::string acquired_at;
};

struct Runtime::Impl {
  ConfigBackend &backend;
  audit::Sink audit;
  std::string state_dir;
  std::string config_dir;
  std::string factory_config;

  mutable std::mutex mu;
  Config running;
  std::optional<ActiveSession> active;
  std::vector<CommitRecord> history;
  // Runtime-owned canonical revision counter; durable across restarts
  // (and backend restarts) so ids never collide or repeat.
  CommitId next_rev = 0;
  PendingConfirm pending;
  // In-memory fallback for a runtime with no state dir; with one,
  // the persisted report is authoritative.
  std::optional<BootReport> last_boot_report;

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

// Directory the config-file surface reads and writes. Defaults beside
// the durable state so a product only has to configure state_dir.
auto ConfigDir(const Runtime::Impl &d) -> std::string {
  if (!d.config_dir.empty()) return d.config_dir;
  if (d.state_dir.empty()) return {};
  return (std::filesystem::path(d.state_dir) / "configs").string();
}

// The rescue configuration lives beside the state, NOT in the configs
// directory, so a factory reset that clears saved configurations cannot
// take it too. Empty when the runtime has no state dir.
auto RescuePath(const Runtime::Impl &d) -> std::string {
  if (d.state_dir.empty()) return {};
  return (std::filesystem::path(d.state_dir) /
          (std::string(kRescueConfigName) + kConfigFileSuffix))
      .string();
}

// Resolve a config name to a path, honouring the reserved rescue name.
auto ResolveConfigPath(const Runtime::Impl &d, const std::string &name)
    -> std::expected<std::string, Error<ConfigFileError>> {
  if (name == kRescueConfigName) {
    auto path = RescuePath(d);
    if (path.empty()) {
      return std::unexpected(Error<ConfigFileError>{
          ConfigFileError::NoConfigDir,
          "this runtime has no state directory"});
    }
    return path;
  }
  return ConfigFilePath(ConfigDir(d), name);
}

// Who holds configure mode. The durable lock is authoritative when the
// runtime has a state dir — it is the only view that sees a session
// held by another process (products that embed confd in the CLI binary
// have one Runtime per logged-in operator).
auto CurrentLock(const Runtime::Impl &d) -> std::optional<EditLock> {
  if (!d.state_dir.empty()) {
    auto held = ReadEditLock(d.state_dir);
    if (held && held->has_value()) return **held;
    return std::nullopt;
  }
  if (!d.active) return std::nullopt;
  EditLock lock;
  lock.holder = d.active->author;
  lock.session_id = d.active->id;
  lock.pid = ::getpid();
  lock.acquired_at = d.active->acquired_at;
  return lock;
}

// Drop the durable lock our active session holds. A no-op when the
// lock was force-stolen in the meantime (ReleaseEditLock only removes
// a lock whose session id matches).
auto ReleaseLock(Runtime::Impl &d) -> void {
  if (d.state_dir.empty() || !d.active) return;
  ReleaseEditLock(d.state_dir, d.active->id);
}

// True while our session still owns the durable lock. A session that
// was displaced by `configure force` must not be able to commit:
// otherwise the operator who was locked out clobbers the work of the
// one who took the lock.
auto StillHoldsLock(const Runtime::Impl &d) -> bool {
  if (d.state_dir.empty() || !d.active) return true;
  auto held = ReadEditLock(d.state_dir);
  if (!held || !held->has_value()) return false;
  return (*held)->session_id == d.active->id;
}

// Adopt the durable store when another process has committed since we
// loaded ours. Called when a session opens, which is the moment before
// a candidate is seeded from running and therefore the last point at
// which adopting someone else's commits is free of surprises.
auto RefreshFromStore(Runtime::Impl &d) -> void {
  if (d.state_dir.empty()) return;
  auto loaded = LoadState(d.state_dir);
  if (!loaded || loaded->next_rev <= d.next_rev) return;
  d.running = std::move(loaded->running);
  d.history = std::move(loaded->history);
  d.next_rev = loaded->next_rev;
  d.pending = loaded->pending;
  d.cv.notify_all();  // the adopted window may need arming
}

auto HandleConfigure(Runtime::Impl &d, const protocol::Request &req,
                     bool force) -> protocol::Response {
  const std::string label = force ? "configure force" : "configure";
  RefreshFromStore(d);

  protocol::Response r;
  r.id = req.id;
  std::optional<EditLock> displaced;
  if (d.active) {
    if (!force) {
      d.Emit(req, label, false, "session busy");
      return ErrorResponse(
          req, "session_busy",
          std::format("configure mode is held by {} (session {})",
                      d.active->author, d.active->id),
          "`configure force` takes it over, discarding their "
          "candidate");
    }
    displaced = CurrentLock(d);
  }

  ActiveSession s;
  s.id = std::format("confd-{}-{}", static_cast<long>(::getpid()),
                     ++g_session_counter);
  s.author = req.user;
  s.acquired_at = audit::NowTimestamp();

  if (!d.state_dir.empty()) {
    EditLock want;
    want.holder = s.author;
    want.session_id = s.id;
    want.pid = static_cast<std::int64_t>(::getpid());
    want.acquired_at = s.acquired_at;
    auto got = AcquireEditLock(d.state_dir, want, force);
    if (!got) {
      const bool held = got.error().code == LockError::Held;
      d.Emit(req, label, false, held ? "lock held" : "lock error");
      return ErrorResponse(
          req, held ? "session_busy" : "lock_error", got.error().message,
          held ? "`configure force` takes it over, discarding their "
                 "candidate"
               : "");
    }
    if (got->stolen_from) {
      displaced = got->stolen_from;
    } else if (got->reclaimed_from && !displaced) {
      // A holder whose process died: reclaimed silently, but recorded,
      // because "the box was wedged in configure mode" is exactly the
      // kind of thing an operator later needs the audit log to explain.
      d.Emit(req, label, true,
             std::format("reclaimed a stale lock from {} (session {})",
                         got->reclaimed_from->holder,
                         got->reclaimed_from->session_id));
    }
  }

  s.candidate.values = d.running;  // seed from running
  d.active = std::move(s);
  r.data = EncodeString(d.active->id);
  if (displaced) {
    d.Emit(req, label, true,
           std::format("took configure mode from {} (session {})",
                       displaced->holder, displaced->session_id));
  } else {
    d.Emit(req, label, true, "ok");
  }
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

// Refusal shared by commit and commit-confirmed: our session lost the
// edit lock to a `configure force` while we were editing.
auto LockLostResponse(Runtime::Impl &d, const protocol::Request &req,
                      const std::string &label) -> protocol::Response {
  d.Emit(req, label, false, "lock lost");
  std::string hint =
      "your candidate is still here; `configure force` takes the lock "
      "back";
  if (auto held = CurrentLock(d)) {
    hint = std::format("{} holds it now (session {}); `configure force` "
                       "takes it back",
                       held->holder, held->session_id);
  }
  return ErrorResponse(
      req, "lock_lost",
      "configure mode was taken over — nothing was committed",
      std::move(hint));
}

auto HandleCommit(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  if (!d.active) {
    d.Emit(req, "commit", false, "no session");
    return ErrorResponse(req, "no_session",
                         "nothing to commit — run `configure`");
  }
  if (!StillHoldsLock(d)) return LockLostResponse(d, req, "commit");
  // Collected BEFORE the apply, because after it the candidate is the
  // running configuration and there is nothing left to compare.
  const auto warnings = d.backend.Warnings(d.active->candidate);
  auto applied = ApplyAndRecord(d, req, d.active->candidate, d.active->author);
  if (!applied) {
    // Keep the session so the operator can fix and retry; running
    // state is untouched.
    d.Emit(req, "commit", false, "apply failed");
    return ErrorResponse(req, "apply_failed", applied.error().message);
  }
  ReleaseLock(d);
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
  std::string body = std::format("commit_id={}", *applied);
  for (const auto &w : warnings) {
    body += std::format("\n!warning={}", w);
  }
  r.data = EncodeString(body);
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
  if (!StillHoldsLock(d)) {
    return LockLostResponse(d, req, "commit confirmed");
  }
  auto applied = ApplyAndRecord(d, req, d.active->candidate, d.active->author);
  if (!applied) {
    d.Emit(req, "commit confirmed", false, "apply failed");
    return ErrorResponse(req, "apply_failed", applied.error().message);
  }
  ReleaseLock(d);
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
    // Discarding the candidate ends the session, so the edit lock goes
    // with it — this is the path the shell takes when the operator
    // leaves configure mode, including on `exit`.
    ReleaseLock(d);
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

auto HandleShowDiff(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  protocol::Response r;
  r.id = req.id;
  if (!d.active) {
    r.data = EncodeString(
        "status=no configure session — nothing to compare\n");
    return r;
  }
  // Candidate vs running, changes only — the Junos `show | compare`
  // view of what commit would do. Same +/~/- markers the adapters
  // already colour for show_commit.
  std::string body;
  for (const auto &[k, v] : d.active->candidate.values) {
    auto it = d.running.find(k);
    if (it == d.running.end()) {
      body += std::format("+{}={}\n", k, v);
    } else if (it->second != v) {
      body += std::format("~{}={} (was {})\n", k, v, it->second);
    }
  }
  for (const auto &[k, v] : d.running) {
    if (!d.active->candidate.values.contains(k)) {
      body += std::format("-{}={}\n", k, v);
    }
  }
  if (body.empty()) {
    body = "status=candidate matches running — nothing to commit\n";
  }
  // Warnings ride along with the diff, not only with the commit: an
  // operator who runs `show diff` first is exactly the operator we
  // want to reach BEFORE they type commit.
  for (const auto &w : d.backend.Warnings(d.active->candidate)) {
    body += std::format("!warning={}\n", w);
  }
  r.data = EncodeString(body);
  return r;
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

// Validate a whole configuration against the product schema. Returns
// the first problem in sorted-path order, so the error a given file
// produces is always the same one. An empty schema (a bare product)
// accepts anything, matching the daemon's pre-schema behaviour.
auto ValidateConfig(const schema::Schema &s, const Config &config)
    -> std::optional<std::string> {
  if (s.root.fields.empty()) return std::nullopt;
  std::vector<std::string> paths;
  paths.reserve(config.size());
  for (const auto &[k, v] : config) paths.push_back(k);
  std::sort(paths.begin(), paths.end());
  for (const auto &path : paths) {
    auto v = schema::ValidatePath(s, path, config.at(path));
    if (!v) return v.error().message;
  }
  return std::nullopt;
}

// What the post-apply overlay found.
struct ReconcileCounts {
  /// Paths the box reports that the applied configuration did not
  /// carry. Routine: the box always knows things config does not say.
  std::size_t added = 0;
  /// Paths where the box disagreed with the value just written to it.
  /// This is the out-of-band-change signal — reality either refused
  /// intent or has already drifted from it.
  std::size_t conflicts = 0;
};

// Post-apply reconcile. Intent wins for every path the applied
// configuration carried — those were just programmed — and the box only
// fills in the paths it said nothing about. The constructor's merge is
// the opposite direction, and correct there, because it runs when
// nobody has re-applied intent yet.
auto ReconcileAfterApply(Runtime::Impl &d, const Config &intent)
    -> ReconcileCounts {
  ReconcileCounts counts;
  d.running = intent;
  for (const auto &[path, value] : d.backend.ReadRunning()) {
    const auto it = intent.find(path);
    if (it == intent.end()) {
      ++counts.added;
      d.running.emplace(path, value);
    } else if (it->second != value) {
      // Intent still wins in `running` — we just wrote it, and the
      // operator's committed value is the truth we report. But the
      // disagreement is recorded, because a box that did not take the
      // value it was given is exactly what an operator needs told.
      ++counts.conflicts;
    }
  }
  return counts;
}

// Map a config-file failure onto a wire error code. BadName and a
// missing config are operator mistakes; the rest are box faults.
auto ConfigFileErrorResponse(const protocol::Request &req,
                             const Error<ConfigFileError> &err)
    -> protocol::Response {
  const char *code = "config_file";
  switch (err.code) {
    case ConfigFileError::BadName:
      code = "bad_args";
      break;
    case ConfigFileError::NotFound:
      code = "not_found";
      break;
    case ConfigFileError::NoConfigDir:
      code = "unavailable";
      break;
    case ConfigFileError::ReadFailed:
    case ConfigFileError::WriteFailed:
    case ConfigFileError::ParseFailed:
      break;
  }
  return ErrorResponse(req, code, err.message);
}

// `save <name>` — the running configuration to a named file. Running,
// not the candidate: a backup should record what the box is actually
// doing, and an uncommitted candidate is by definition not that.
auto HandleSave(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  if (req.args.empty()) {
    d.Emit(req, "save", false, "bad args");
    return ErrorResponse(req, "bad_args", "usage: save <name>");
  }
  auto path = ResolveConfigPath(d, req.args[0]);
  if (!path) {
    d.Emit(req, "save", false, path.error().message);
    return ConfigFileErrorResponse(req, path.error());
  }
  if (auto w = WriteConfigFile(*path, d.running); !w) {
    d.Emit(req, "save", false, w.error().message);
    return ConfigFileErrorResponse(req, w.error());
  }
  d.Emit(req, "save", true, std::format("wrote {}", *path));
  protocol::Response r;
  r.id = req.id;
  r.data = EncodeString(std::format("saved={} paths={}\n", req.args[0],
                                    d.running.size()));
  return r;
}

// `load merge|replace <name>` / `load factory` — a config file into the
// candidate. Never straight onto the box: a loaded file goes through
// the same commit the operator would type by hand, so `show diff`,
// commit-confirmed and rollback all still apply.
auto HandleLoad(Runtime::Impl &d, const protocol::Request &req,
                const std::string &mode) -> protocol::Response {
  const auto label = std::format("load {}", mode);
  if (!d.active || req.session_id.value_or("") != d.active->id) {
    d.Emit(req, label, false, "no session");
    return ErrorResponse(req, "no_session", "run `configure` first");
  }

  Config loaded;
  std::string source;
  if (mode == "factory") {
    std::error_code ec;
    if (!d.factory_config.empty() &&
        std::filesystem::exists(d.factory_config, ec)) {
      auto c = ReadConfigFile(d.factory_config);
      if (!c) {
        d.Emit(req, label, false, c.error().message);
        return ConfigFileErrorResponse(req, c.error());
      }
      loaded = std::move(*c);
      source = d.factory_config;
    } else {
      // No shipped defaults file: the factory configuration is the
      // empty one, which on commit resets the box to its own defaults.
      source = "<none>";
    }
  } else {
    if (req.args.empty()) {
      d.Emit(req, label, false, "bad args");
      return ErrorResponse(req, "bad_args",
                           std::format("usage: load {} <name>", mode));
    }
    auto path = ResolveConfigPath(d, req.args[0]);
    if (!path) {
      d.Emit(req, label, false, path.error().message);
      return ConfigFileErrorResponse(req, path.error());
    }
    auto c = ReadConfigFile(*path);
    if (!c) {
      d.Emit(req, label, false, c.error().message);
      return ConfigFileErrorResponse(req, c.error());
    }
    loaded = std::move(*c);
    source = req.args[0];
  }

  // Validate the whole file before touching the candidate: a file that
  // is half-loaded and half-rejected is worse than one refused
  // outright.
  if (auto bad = ValidateConfig(d.backend.Schema(), loaded)) {
    d.Emit(req, label, false, "validation");
    return ErrorResponse(req, "validation",
                         std::format("{}: {}", source, *bad));
  }

  if (mode == "merge") {
    for (const auto &[k, v] : loaded) {
      d.active->candidate.values[k] = v;
    }
  } else {
    // replace and factory both mean "the candidate is exactly this".
    d.active->candidate.values = loaded;
  }
  d.Emit(req, label, true,
         std::format("{} ({} paths)", source, loaded.size()));
  protocol::Response r;
  r.id = req.id;
  r.data = EncodeString(std::format("loaded={} mode={} paths={}\n", source,
                                    mode, loaded.size()));
  return r;
}

// `rollback rescue` — get back to the last known-good configuration
// NOW. Applies immediately and records a commit, exactly like
// `rollback previous` and `rollback to <id>`; Junos stages it in the
// candidate instead, but consistency inside one product beats parity
// with another, and the verb exists for use under duress.
auto HandleRollbackRescue(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  const std::string label = "rollback rescue";
  const auto path = RescuePath(d);
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec)) {
    d.Emit(req, label, false, "no rescue configuration");
    return ErrorResponse(req, "not_found",
                         "no rescue configuration has been saved",
                         "`save rescue` stores the running configuration "
                         "as the rescue slot");
  }
  auto rescue = ReadConfigFile(path);
  if (!rescue) {
    d.Emit(req, label, false, rescue.error().message);
    return ConfigFileErrorResponse(req, rescue.error());
  }
  if (auto bad = ValidateConfig(d.backend.Schema(), *rescue)) {
    d.Emit(req, label, false, "validation");
    return ErrorResponse(req, "validation",
                         std::format("rescue config: {}", *bad));
  }
  Candidate target;
  target.values = std::move(*rescue);
  auto applied = ApplyAndRecord(d, req, target, req.user);
  if (!applied) {
    d.Emit(req, label, false, "apply failed");
    return ErrorResponse(req, "apply_failed", applied.error().message);
  }
  d.Emit(req, label, true, std::format("restored rescue as commit {}",
                                       *applied));
  protocol::Response r;
  r.id = req.id;
  r.data = EncodeString(std::format("commit_id={}", *applied));
  return r;
}

auto HandleShowConfigs(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  protocol::Response r;
  r.id = req.id;
  std::string body;
  const auto names = ListConfigFiles(ConfigDir(d));
  for (const auto &name : names) {
    body += std::format("saved={}\n", name);
  }
  if (names.empty()) body += "saved=<none>\n";
  // The rescue slot is listed separately because it is not in the
  // configs directory and is not one of the names above.
  std::error_code rescue_ec;
  const auto rescue = RescuePath(d);
  body += std::format(
      "rescue={}\n",
      !rescue.empty() && std::filesystem::exists(rescue, rescue_ec)
          ? "saved"
          : "<none>");
  std::error_code ec;
  const bool have_factory =
      !d.factory_config.empty() &&
      std::filesystem::exists(d.factory_config, ec);
  body += std::format("factory={}\n",
                      have_factory ? d.factory_config : "<none>");
  r.data = EncodeString(body);
  return r;
}

// `show system boot` — what the last boot-restore did.
//
// The load-bearing line is `ran_this_boot`. A boot where the unit never
// started leaves the previous boot's report in place, which would read
// as a perfectly healthy boot; comparing the recorded kernel boot id
// against the live one is what turns that silent case into a visible
// one. It is not hypothetical: a systemd ordering cycle deleted the
// boot job on 2 of 50 power cuts in Phase 0 testing.
auto HandleShowSystemBoot(Runtime::Impl &d, const protocol::Request &req)
    -> protocol::Response {
  protocol::Response r;
  r.id = req.id;

  std::optional<BootReport> rep = d.last_boot_report;
  if (!d.state_dir.empty()) {
    if (auto loaded = LoadBootReport(d.state_dir);
        loaded && loaded->has_value()) {
      rep = **loaded;
    } else if (!loaded) {
      return ErrorResponse(req, "boot_report", loaded.error().message);
    }
  }
  if (!rep) {
    r.data = EncodeString(
        "status=no boot report — boot-restore has never run here\n");
    return r;
  }

  const auto live = CurrentBootId();
  const bool this_boot = IsFromCurrentBoot(*rep, live);
  std::string body;
  body += std::format("ran_this_boot={}\n", this_boot ? "yes" : "no");
  if (!this_boot) {
    // Kept short: the renderer sizes the field column against the
    // widest value, so a long line here squeezes every field name into
    // an unreadable stub.
    body += "warning=values below are from an EARLIER boot\n";
    body += "hint=journalctl -b -u einheit-s5-boot\n";
    body += "hint2=look for a job deleted to break an ordering cycle\n";
  }
  body += std::format("outcome={}\n", rep->ok ? "ok" : "FAILED");
  body += std::format("at={}\n", rep->timestamp.empty() ? "<unknown>"
                                                        : rep->timestamp);
  body += std::format("applied_revision={}\n", rep->applied_revision);
  body += std::format("paths={}\n", rep->paths);
  if (rep->seeded_factory) body += "seeded_factory=yes\n";
  if (rep->reverted_pending) body += "reverted_unconfirmed_commit=yes\n";
  body += std::format("reconcile_added={}\n", rep->reconcile_added);
  body += std::format("config_divergence={}\n", rep->reconcile_conflicts);
  body += std::format("duration_ms={}\n", rep->duration_ms);
  for (const auto &s : rep->steps) {
    body += std::format("step.{}={} ({}ms){}{}\n", s.name,
                        s.ok ? "ok" : "FAILED", s.duration_ms,
                        s.detail.empty() ? "" : " — ", s.detail);
  }
  r.data = EncodeString(body);
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
  // Who holds configure mode is state, so it is inspectable — an
  // operator who was just refused needs to see the holder without
  // having to read the refusal again.
  if (auto lock = CurrentLock(d)) {
    txt += std::format("lock_holder={}\nlock_session={}\nlock_since={}\n",
                       lock->holder.empty() ? "<unknown>" : lock->holder,
                       lock->session_id, lock->acquired_at.empty()
                                             ? "<unknown>"
                                             : lock->acquired_at);
    if (lock->pid > 0) {
      txt += std::format("lock_pid={}\n", lock->pid);
    }
  } else {
    txt += "lock_holder=<none>\n";
  }
  r.data = EncodeString(txt);
  return r;
}

}  // namespace

Runtime::Runtime(ConfigBackend &backend, RuntimeOptions opts)
    : impl_(std::make_unique<Impl>(backend)) {
  impl_->audit = std::move(opts.audit);
  impl_->state_dir = std::move(opts.state_dir);
  impl_->config_dir = std::move(opts.config_dir);
  impl_->factory_config = std::move(opts.factory_config);

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
  } else {
    // Reconcile intent with reality — the documented purpose of
    // ReadRunning. The box may have changed while confd was down,
    // and the config surface may have grown paths the stored
    // running predates; unreconciled, those paths are invisible to
    // candidate seeding, so commits freeze incomplete candidates
    // and a later rollback silently fails to restore them. Reality
    // wins per key ("running" means the box; intent lives in
    // history); stored-only keys survive, covering write-only
    // config the box cannot report back.
    for (auto &[path, value] : backend.ReadRunning()) {
      impl_->running[path] = value;
    }
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
  // The edit lock dies with the session: a runtime going down must not
  // leave configure mode wedged for the next process. A hard crash
  // skips this, which is what the stale-pid reclaim in AcquireEditLock
  // is for.
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    ReleaseLock(*impl_);
  }
}

auto Runtime::ApplyRunningAtBoot(std::vector<BootStep> prior_steps)
    -> std::expected<BootApplyResult, Error<ApplyError>> {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto &d = *impl_;
  protocol::Request note;
  note.command = "apply_boot";
  note.user = "confd";

  const auto t0 = std::chrono::steady_clock::now();
  const auto elapsed_ms = [&t0]() -> std::int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  };

  BootReport rep;
  rep.boot_id = CurrentBootId();
  rep.timestamp = audit::NowTimestamp();
  rep.steps = std::move(prior_steps);
  for (const auto &s : rep.steps) {
    // A product subsystem that failed makes the boot not-ok even if the
    // configuration apply below then succeeds — a switch whose fabric
    // did not come up has not had a good boot.
    if (!s.ok) rep.ok = false;
  }

  // EVERY exit from here writes the report. "The apply failed" and
  // "there was nothing to apply" are outcomes an operator has to be
  // able to see afterwards just as much as a success is.
  const auto finish = [&](bool ok, std::string detail) -> void {
    BootStep step;
    step.name = "config-apply";
    step.ok = ok;
    step.detail = std::move(detail);
    step.duration_ms = elapsed_ms();
    rep.steps.push_back(std::move(step));
    if (!ok) rep.ok = false;
    rep.duration_ms = elapsed_ms();
    d.last_boot_report = rep;
    if (d.state_dir.empty()) return;
    if (auto w = SaveBootReport(d.state_dir, rep); !w) {
      d.Emit(note, "boot-report", false, w.error().message);
    }
  };

  BootApplyResult out;
  if (d.pending.armed) {
    // Boot is the confirm deadline. AutoRevert applies the pre-confirm
    // configuration, records it and disarms, so the restore below
    // targets the last *confirmed* intent.
    AutoRevert(d);
    out.reverted_pending = true;
    rep.reverted_pending = true;
    d.cv.notify_all();
  }
  if (d.history.empty()) {
    // Nothing committed yet — a box out of the factory. Seed the
    // shipped defaults as the first commit, because an unconfigured
    // switch is not a neutral state: on Linux every switch port comes
    // up administratively DOWN, so "no configuration" ships a box that
    // forwards nothing. This is the same file `load factory` stages,
    // and recording it as a commit gives `show config`, `show commits`
    // and rollback a real floor to stand on.
    std::error_code ec;
    if (d.factory_config.empty() ||
        !std::filesystem::exists(d.factory_config, ec)) {
      d.Emit(note, "apply-boot", true, "no committed configuration");
      finish(true, "no committed configuration to restore");
      return out;
    }
    auto factory = ReadConfigFile(d.factory_config);
    if (!factory) {
      d.Emit(note, "apply-boot", false, factory.error().message);
      finish(false, std::format("factory config {}: {}", d.factory_config,
                                factory.error().message));
      return std::unexpected(Error<ApplyError>{
          ApplyError::ValidationFailed,
          std::format("factory config {}: {}", d.factory_config,
                      factory.error().message)});
    }
    if (auto bad = ValidateConfig(d.backend.Schema(), *factory)) {
      d.Emit(note, "apply-boot", false, *bad);
      finish(false,
             std::format("factory config {}: {}", d.factory_config, *bad));
      return std::unexpected(Error<ApplyError>{
          ApplyError::ValidationFailed,
          std::format("factory config {}: {}", d.factory_config, *bad)});
    }
    Candidate seed;
    seed.values = std::move(*factory);
    auto applied = ApplyAndRecord(d, note, seed, "confd-factory");
    if (!applied) {
      d.Emit(note, "apply-boot", false, applied.error().message);
      finish(false, applied.error().message);
      return std::unexpected(applied.error());
    }
    out.applied = true;
    out.seeded_factory = true;
    out.paths = seed.values.size();
    out.commit = *applied;
    const auto counts = ReconcileAfterApply(d, seed.values);
    d.Persist(note);
    rep.applied_revision = *applied;
    rep.paths = out.paths;
    rep.seeded_factory = true;
    rep.reconcile_added = counts.added;
    rep.reconcile_conflicts = counts.conflicts;
    d.Emit(note, "apply-boot", true,
           std::format("seeded factory defaults as commit {} ({} paths)",
                       *applied, out.paths));
    finish(true, std::format("seeded factory defaults as commit {}",
                             *applied));
    return out;
  }

  // A copy, not a reference into d.history: Apply is free to take a
  // while, and ApplyAndRecord elsewhere may reallocate the vector.
  const Candidate target = d.history.back().candidate;
  const CommitId commit = d.history.back().id;
  auto applied = d.backend.Apply(target);
  if (!applied) {
    d.Emit(note, "apply-boot", false, applied.error().message);
    finish(false, applied.error().message);
    return std::unexpected(applied.error());
  }
  out.applied = true;
  out.paths = target.values.size();
  out.commit = commit;
  const auto counts = ReconcileAfterApply(d, target.values);
  d.Persist(note);
  rep.applied_revision = commit;
  rep.paths = out.paths;
  rep.reconcile_added = counts.added;
  rep.reconcile_conflicts = counts.conflicts;
  d.Emit(note, "apply-boot", true,
         std::format("restored commit {} ({} paths)", commit, out.paths));
  if (counts.conflicts > 0) {
    // Out-of-band change: the box did not come back holding what was
    // committed. Audited separately from the apply so it is greppable.
    d.Emit(note, "config-divergence", false,
           std::format("{} path(s) diverged from commit {} at boot",
                       counts.conflicts, commit));
  }
  finish(true, std::format("restored commit {}", commit));
  return out;
}

auto Runtime::LastBootReport() const -> std::optional<BootReport> {
  std::lock_guard<std::mutex> lk(impl_->mu);
  // Prefer the persisted copy: the process serving `show system boot`
  // is an interactive CLI that never ran a boot apply itself, so its
  // in-memory copy is empty by construction.
  if (!impl_->state_dir.empty()) {
    if (auto r = LoadBootReport(impl_->state_dir); r && r->has_value()) {
      return **r;
    }
  }
  return impl_->last_boot_report;
}

auto Runtime::EditLockState() const -> std::optional<EditLock> {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return CurrentLock(*impl_);
}

auto Runtime::HandleRequest(const protocol::Request &req)
    -> protocol::Response {
  std::lock_guard<std::mutex> lk(impl_->mu);
  auto &d = *impl_;

  if (req.command == "configure") {
    return HandleConfigure(d, req, /*force=*/false);
  }
  if (req.command == "configure_force") {
    return HandleConfigure(d, req, /*force=*/true);
  }
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
  if (req.command == "rollback_rescue") {
    return HandleRollbackRescue(d, req);
  }
  if (req.command == "rollback_previous") {
    return HandleRollback(d, req, "previous");
  }
  if (req.command == "rollback_to") {
    return HandleRollback(d, req,
                          req.args.empty() ? std::string() : req.args[0]);
  }
  if (req.command == "save") return HandleSave(d, req);
  if (req.command == "load_merge") return HandleLoad(d, req, "merge");
  if (req.command == "load_replace") return HandleLoad(d, req, "replace");
  if (req.command == "load_factory") return HandleLoad(d, req, "factory");
  if (req.command == "show_configs") return HandleShowConfigs(d, req);
  if (req.command == "show_system_boot") {
    return HandleShowSystemBoot(d, req);
  }
  if (req.command == "show_config") return HandleShowConfig(d, req);
  if (req.command == "show_diff") return HandleShowDiff(d, req);
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
