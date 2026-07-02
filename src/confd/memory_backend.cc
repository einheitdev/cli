/// @file memory_backend.cc
/// @brief MemoryBackend implementation — fake programmable hardware.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/confd/memory_backend.h"

#include <memory>
#include <mutex>
#include <utility>

namespace einheit::cli::confd {

MemoryBackend::MemoryBackend(std::shared_ptr<const schema::Schema> schema)
    : schema_(schema ? std::move(schema)
                     : std::make_shared<const schema::Schema>()) {}

auto MemoryBackend::Apply(const Candidate &candidate)
    -> std::expected<CommitId, Error<ApplyError>> {
  std::lock_guard<std::mutex> lk(mu_);
  if (fail_next_) {
    fail_next_ = false;
    return std::unexpected(Error<ApplyError>{ApplyError::HardwareRejected,
                                             "simulated hardware rejection"});
  }
  // Program the box: the device now holds exactly the candidate.
  device_ = candidate.values;
  ++apply_count_;
  return ++rev_;
}

auto MemoryBackend::ReadRunning() -> Config {
  std::lock_guard<std::mutex> lk(mu_);
  return device_;
}

auto MemoryBackend::Schema() const -> const schema::Schema & {
  return *schema_;
}

auto MemoryBackend::DeviceState() const -> Config {
  std::lock_guard<std::mutex> lk(mu_);
  return device_;
}

auto MemoryBackend::ApplyCount() const -> int {
  std::lock_guard<std::mutex> lk(mu_);
  return apply_count_;
}

auto MemoryBackend::FailNextApply() -> void {
  std::lock_guard<std::mutex> lk(mu_);
  fail_next_ = true;
}

}  // namespace einheit::cli::confd
