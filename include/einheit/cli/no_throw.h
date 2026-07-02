/// @file no_throw.h
/// @brief `NoThrow` — a single exception-firewall for C-ABI callback
/// boundaries.
///
/// Any callback the framework hands to a C library (replxx's
/// completion / hint / key-binding hooks) runs inside that library's
/// own input loop. A C++ exception unwinding across that C frame calls
/// `std::terminate` — the whole CLI dies silently (gap #7).
///
/// `NoThrow(fn)` wraps a callable so no exception can escape it: a
/// throw is swallowed and the callable's return type is
/// default-constructed instead (`completions_t{}`, `hints_t{}`,
/// `ACTION_RESULT{}` == CONTINUE, or nothing for `void`). Applying it
/// once at the registration site means a *new* callback can't
/// reintroduce the crash — the guard is structural, not remembered
/// per-site.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_NO_THROW_H_
#define INCLUDE_EINHEIT_CLI_NO_THROW_H_

#include <type_traits>
#include <utility>

namespace einheit::cli {

/// Wrap `fn` so that no exception it throws can propagate to the
/// caller. On any exception the wrapper returns a default-constructed
/// value of `fn`'s return type (or nothing, if it returns `void`).
///
/// The returned callable is move-only-friendly and forwards all
/// arguments. Intended for callbacks registered with a C library that
/// cannot tolerate a C++ exception crossing its frames.
///
/// @param fn Any invocable. Its return type must be
///   default-constructible (or `void`) so a safe fallback exists.
/// @returns A callable with the same call signature that never throws.
template <typename F>
[[nodiscard]] auto NoThrow(F fn) {
  return [fn = std::move(fn)](auto &&...args) mutable noexcept
             -> std::invoke_result_t<F &, decltype(args)...> {
    using R = std::invoke_result_t<F &, decltype(args)...>;
    try {
      return fn(std::forward<decltype(args)>(args)...);
    } catch (...) {
      // A failed callback is a no-op, never a crash. The safe
      // fallback is the return type's default: empty completion /
      // hint list, or ACTION_RESULT::CONTINUE for a key binding.
      if constexpr (!std::is_void_v<R>) {
        return R{};
      }
    }
  };
}

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_NO_THROW_H_
