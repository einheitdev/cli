/// @file oneshot.cc
/// @brief One-shot dispatch for scripted CLI invocations.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/transport/oneshot.h"

#include <chrono>

namespace einheit::cli::transport {

auto RunOneshot(Transport &t, const protocol::Request &req)
    -> std::expected<protocol::Response, Error<TransportError>> {
  using namespace std::chrono_literals;
  return t.SendRequest(req, 30s);
}

}  // namespace einheit::cli::transport
