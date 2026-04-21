/// @file adapter.h
/// @brief Example adapter — the template every real product adapter
/// (g-gateway, hd-relay, f-standalone) follows. Declares commands,
/// provides renderers, points at daemon endpoints.
// Copyright (c) 2026 Einheit Networks

#ifndef ADAPTERS_EXAMPLE_ADAPTER_H_
#define ADAPTERS_EXAMPLE_ADAPTER_H_

#include <memory>
#include <string>
#include <vector>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/schema.h"

namespace einheit::adapters::example {

/// Construct an example ProductAdapter. Suitable as a template for
/// copy/paste when starting a new product adapter.
/// @returns Owned ProductAdapter.
auto NewExampleAdapter() -> std::unique_ptr<einheit::cli::ProductAdapter>;

}  // namespace einheit::adapters::example

#endif  // ADAPTERS_EXAMPLE_ADAPTER_H_
