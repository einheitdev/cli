/// @file session.cc
/// @brief Candidate-config session state helpers.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/session.h"

namespace einheit::cli {

auto ClearSession(Session &s) -> void {
  s.in_configure = false;
  s.session_id.reset();
  s.candidate_hash.reset();
  s.confirm_deadline.reset();
}

}  // namespace einheit::cli
