/// @file adapter.h
/// @brief Hyper-DERP relay adapter — the canonical einheit
/// protocol consumer. First real product adapter, serves as the
/// template for g-gateway and f-standalone.
// Copyright (c) 2026 Einheit Networks

#ifndef ADAPTERS_HD_RELAY_ADAPTER_H_
#define ADAPTERS_HD_RELAY_ADAPTER_H_

#include <memory>

#include "einheit/cli/adapter.h"

namespace einheit::adapters::hd_relay {

/// Construct an hd-relay ProductAdapter. The adapter is a thin
/// declarative object: schema, command specs, and renderers. All
/// daemon-side state lives in hyper-derp itself.
/// @returns Owned ProductAdapter.
auto NewHdRelayAdapter()
    -> std::unique_ptr<einheit::cli::ProductAdapter>;

}  // namespace einheit::adapters::hd_relay

#endif  // ADAPTERS_HD_RELAY_ADAPTER_H_
