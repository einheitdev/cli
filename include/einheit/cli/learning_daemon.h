/// @file learning_daemon.h
/// @brief In-process stub daemon that backs `einheit --learn`.
///
/// Binds REP + PUB on a tmpdir `ipc://` path, accepts the real wire
/// protocol, and maintains a small in-memory model so `configure`,
/// `set`, `commit`, `rollback`, and `show config` all behave as a
/// coherent arc. Every layer of the framework is exercised — this
/// is not a mock, it's the real protocol talking to a pretend
/// daemon.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_LEARNING_DAEMON_H_
#define INCLUDE_EINHEIT_CLI_LEARNING_DAEMON_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::learning {

/// Tiny candidate config — a flat key/value map is enough to
/// demonstrate the commit machinery without modelling a real
/// product schema. Adapters that want richer fake state can grow
/// this over time.
struct CandidateState {
  std::unordered_map<std::string, std::string> values;
};

/// Running daemon with background REP/PUB threads. Destructor
/// joins; safe to construct + drop from main().
class LearningDaemon {
 public:
  /// Construct with no tracing and no schema validation.
  LearningDaemon();
  /// Construct with a trace sink. Every decoded Request + outgoing
  /// Response gets one human-readable line written to `trace`. Pass
  /// `&std::cerr` from `main` to show wire traffic on stderr while
  /// the REPL runs on stdout. The stream must outlive the daemon.
  explicit LearningDaemon(std::ostream *trace);
  /// Construct with a trace sink and a schema. `set` and `delete`
  /// are validated against the schema before touching the candidate
  /// — operators see schema-level error messages (bad path, out of
  /// range, enum mismatch) the same way a real daemon would emit
  /// them. The schema must outlive the daemon.
  LearningDaemon(std::ostream *trace,
                 std::shared_ptr<const schema::Schema> schema);
  ~LearningDaemon();

  LearningDaemon(const LearningDaemon &) = delete;
  auto operator=(const LearningDaemon &) -> LearningDaemon & = delete;

  /// `ipc://...` path the REP socket is bound to. Hand this to the
  /// transport's `control_endpoint`.
  auto ControlEndpoint() const -> const std::string &;

  /// `ipc://...` path the PUB socket is bound to.
  auto EventEndpoint() const -> const std::string &;

  // Implementation detail, exposed for the .cc file's free-function
  // helpers. Treat as opaque from outside.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace einheit::cli::learning

#endif  // INCLUDE_EINHEIT_CLI_LEARNING_DAEMON_H_
