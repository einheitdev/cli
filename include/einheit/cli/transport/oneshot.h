/// @file oneshot.h
/// @brief One-shot dispatch helper for scripted / non-interactive use.
///
/// `einheit show tunnels` run from a script parses args, opens a
/// transport, sends one request, renders the response, exits. No REPL.
/// This header wires that flow without duplicating the interactive
/// shell's command routing.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_TRANSPORT_ONESHOT_H_
#define INCLUDE_EINHEIT_CLI_TRANSPORT_ONESHOT_H_

#include <expected>
#include <string>
#include <vector>

#include "einheit/cli/error.h"
#include "einheit/cli/protocol/envelope.h"
#include "einheit/cli/transport/transport.h"

namespace einheit::cli::transport {

/// Run one Request against an already-connected Transport and return
/// the matching Response. Callers render the result themselves.
/// @param t Connected transport.
/// @param req Request to send.
/// @returns Response or TransportError.
auto RunOneshot(Transport &t, const protocol::Request &req)
    -> std::expected<protocol::Response, Error<TransportError>>;

}  // namespace einheit::cli::transport

#endif  // INCLUDE_EINHEIT_CLI_TRANSPORT_ONESHOT_H_
