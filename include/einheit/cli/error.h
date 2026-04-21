/// @file error.h
/// @brief Shared Error<E> template used by every fallible call in the
/// framework. Pairs with std::expected<T, Error<E>>.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_ERROR_H_
#define INCLUDE_EINHEIT_CLI_ERROR_H_

#include <string>

namespace einheit::cli {

/// Uniform error wrapper: a typed enum plus a human-readable message.
/// @tparam ErrorCodeEnum A strongly-typed enum declared per module
/// (e.g. TransportError, SchemaError, ShellError).
template <typename ErrorCodeEnum>
struct Error {
  /// Machine-readable code. Callers branch on this.
  ErrorCodeEnum code;
  /// Human-readable context. Rendered in audit logs and diagnostics.
  std::string message;
};

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_ERROR_H_
