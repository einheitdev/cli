/// @file fake_curve_daemon.cc
/// @brief Out-of-line keypair generator (calls into libzmq).
// Copyright (c) 2026 Einheit Networks

#include "tests/fake_curve_daemon.h"

#include <array>
#include <stdexcept>

#include <zmq.h>

namespace einheit::cli::testing {

auto NewCurveKeyPair() -> CurveKeyPair {
  std::array<char, 41> pub{};
  std::array<char, 41> sec{};
  const int rc = zmq_curve_keypair(pub.data(), sec.data());
  if (rc != 0) {
    throw std::runtime_error("zmq_curve_keypair failed");
  }
  return CurveKeyPair{pub.data(), sec.data()};
}

}  // namespace einheit::cli::testing
