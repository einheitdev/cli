/// @file adapter.h
/// @brief takt CLI adapter — connects to takt-service
/// via ZMQ for pipeline orchestration commands.
// Copyright (c) 2026 Einheit Networks

#ifndef ADAPTERS_TAKT_ADAPTER_H_
#define ADAPTERS_TAKT_ADAPTER_H_

#include <memory>
#include <string>
#include <vector>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/schema.h"

namespace einheit::adapters::takt {

/// Construct a takt ProductAdapter.
/// @returns Owned ProductAdapter.
auto NewTaktAdapter()
    -> std::unique_ptr<einheit::cli::ProductAdapter>;

}  // namespace einheit::adapters::takt

#endif  // ADAPTERS_TAKT_ADAPTER_H_
