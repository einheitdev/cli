/// @file engine.cc
/// @brief Reusable command-execution engine implementation.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/engine.h"

#include <chrono>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "einheit/cli/audit.h"

namespace einheit::cli::engine {
namespace {

auto MakeError(EngineError code, std::string message) -> Error<EngineError> {
  return Error<EngineError>{code, std::move(message)};
}

// Extract the daemon-issued session id out of a configure/commit
// Response. The framework runtime encodes it in the `data` blob — any
// non-empty string there is accepted as the id. Adapters with richer
// response shapes decode via their own renderers.
auto ExtractSessionId(const std::vector<std::uint8_t> &bytes)
    -> std::optional<std::string> {
  if (bytes.empty()) return std::nullopt;
  return std::string(bytes.begin(), bytes.end());
}

// Fold a successful lifecycle response back into session state. Kept
// in one place so every front-end threads the session identically.
auto UpdateSession(Session &session, const CommandSpec &spec,
                   const protocol::Response &resp) -> void {
  if (resp.status != protocol::ResponseStatus::Ok) return;
  if (spec.wire_command == "configure") {
    session.in_configure = true;
    session.session_id = ExtractSessionId(resp.data);
  } else if (spec.wire_command == "commit" ||
             spec.wire_command == "commit_confirmed" ||
             spec.path == "rollback candidate") {
    // commit-confirmed closes the candidate session just like a plain
    // commit; the auto-revert window is tracked server-side.
    ClearSession(session);
  }
  // rollback previous / rollback to N re-apply a committed revision
  // without touching the active candidate session, so they leave the
  // session untouched.
}

auto Emit(const Context &ctx, const audit::Record &rec) -> void {
  if (ctx.audit) ctx.audit(rec);
}

}  // namespace

auto NewRequestId() -> std::string {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  return std::format("{:016x}{:016x}", rng(), rng());
}

auto BuildRequest(const Context &ctx, const ParsedCommand &parsed)
    -> protocol::Request {
  protocol::Request req;
  req.id = NewRequestId();
  req.command = parsed.spec->wire_command;
  req.args = parsed.args;
  audit::StampIdentity(ctx.caller, req.user, req.role);
  if (parsed.spec->requires_session && ctx.session && ctx.session->session_id) {
    req.session_id = *ctx.session->session_id;
  }
  return req;
}

auto Execute(Context &ctx, const ParsedCommand &parsed)
    -> std::expected<ExecOutcome, Error<EngineError>> {
  const auto &spec = *parsed.spec;

  audit::Record rec;
  rec.timestamp = audit::NowTimestamp();
  rec.user = ctx.caller.user;
  audit::StampIdentity(ctx.caller, rec.user, rec.role);
  rec.command = spec.path;
  rec.wire_command = spec.wire_command;
  rec.args = parsed.args;
  if (ctx.session) rec.session_id = ctx.session->session_id;

  // The engine executes wire commands only; framework-local verbs are
  // a front-end concern.
  if (spec.wire_command.empty()) {
    rec.ok = false;
    rec.outcome = "not a wire command";
    Emit(ctx, rec);
    return std::unexpected(
        MakeError(EngineError::NotAWireCommand,
                  std::format("`{}` is a framework-local verb, not a wire "
                              "command",
                              spec.path)));
  }

  // One session-gating code path for every front-end (gap #8).
  if (spec.requires_session && (!ctx.session || !ctx.session->in_configure)) {
    rec.ok = false;
    rec.outcome = "session required";
    Emit(ctx, rec);
    return std::unexpected(MakeError(
        EngineError::SessionRequired,
        std::format("command requires 'configure' session: {}", spec.path)));
  }

  if (!ctx.tx) {
    rec.ok = false;
    rec.outcome = "no transport";
    Emit(ctx, rec);
    return std::unexpected(
        MakeError(EngineError::TransportUnavailable, "no transport attached"));
  }

  auto req = BuildRequest(ctx, parsed);
  const auto t0 = std::chrono::steady_clock::now();
  auto resp = ctx.tx->SendRequest(req, ctx.timeout);
  ExecOutcome out;
  out.rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0);

  if (!resp) {
    out.wire = resp.error().code == transport::TransportError::Timeout
                   ? WireStatus::Timeout
                   : WireStatus::Failed;
    out.error_message = resp.error().message;
    rec.ok = false;
    rec.outcome =
        out.wire == WireStatus::Timeout ? "timeout" : "transport error";
    Emit(ctx, rec);
    return out;
  }

  out.wire = WireStatus::Ok;
  if (ctx.session) UpdateSession(*ctx.session, spec, *resp);

  rec.ok = resp->status == protocol::ResponseStatus::Ok;
  if (rec.ok) {
    rec.outcome = "ok";
  } else if (resp->error) {
    rec.outcome = resp->error->code;
  } else {
    rec.outcome = "error";
  }
  // Reflect the session id the daemon just issued (configure) or
  // cleared (commit/rollback) in the record.
  if (ctx.session) rec.session_id = ctx.session->session_id;
  Emit(ctx, rec);

  out.response = std::move(*resp);
  return out;
}

}  // namespace einheit::cli::engine
