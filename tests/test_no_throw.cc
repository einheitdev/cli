/// @file test_no_throw.cc
/// @brief Tests for the NoThrow C-ABI callback firewall (gap #7).
///
/// The failure this prevents: a C++ exception thrown inside a callback
/// registered with replxx (a C library) unwinds across a C frame and
/// calls std::terminate — the whole CLI dies. NoThrow must contain any
/// throw and yield a safe default so the same wrapper can guard every
/// registered callback, once, at the registration site.
// Copyright (c) 2026 Einheit Networks

#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "einheit/cli/no_throw.h"

namespace einheit::cli {
namespace {

// The wrapped callable must be noexcept — the whole point is that it
// cannot let an exception reach a C caller.
TEST(NoThrow, WrapperIsNoexcept) {
  auto guarded = NoThrow([]() -> int { return 1; });
  static_assert(noexcept(guarded()),
                "NoThrow wrapper must be noexcept");
  EXPECT_EQ(guarded(), 1);
}

// A throwing value-returning callable yields the default-constructed
// return value instead of propagating.
TEST(NoThrow, ThrowingCallableReturnsDefault) {
  auto guarded = NoThrow([]() -> int {
    throw std::runtime_error("boom");
  });
  EXPECT_EQ(guarded(), 0);
}

// Container returns collapse to empty on throw — mirrors replxx's
// completions_t / hints_t.
TEST(NoThrow, ThrowingContainerCallableReturnsEmpty) {
  auto guarded = NoThrow([]() -> std::vector<std::string> {
    throw std::logic_error("nope");
  });
  EXPECT_TRUE(guarded().empty());
}

// A void callback (like the `?` key binding's inner work) simply
// swallows the throw.
TEST(NoThrow, ThrowingVoidCallableIsSwallowed) {
  bool reached_after = false;
  auto guarded = NoThrow([]() -> void {
    throw std::runtime_error("boom");
  });
  guarded();  // must not throw
  reached_after = true;
  EXPECT_TRUE(reached_after);
}

// The happy path forwards arguments and returns the real value —
// NoThrow is transparent when nothing throws.
TEST(NoThrow, ForwardsArgsAndReturnsValueWhenNoThrow) {
  auto guarded = NoThrow([](int a, int b) -> int { return a + b; });
  EXPECT_EQ(guarded(3, 4), 7);
}

// Reference out-parameters (replxx passes `int& len`, `Color& color`)
// are still mutated on the success path.
TEST(NoThrow, MutatesReferenceOutParamsOnSuccess) {
  auto guarded = NoThrow([](int &out) -> bool {
    out = 42;
    return true;
  });
  int sink = 0;
  EXPECT_TRUE(guarded(sink));
  EXPECT_EQ(sink, 42);
}

// An enum return whose first enumerator is the safe "keep going"
// action (replxx ACTION_RESULT::CONTINUE == 0) defaults correctly on
// throw.
TEST(NoThrow, EnumReturnDefaultsToFirstEnumerator) {
  enum class Action { kContinue, kReturn, kBail };
  auto guarded = NoThrow([]() -> Action {
    throw std::runtime_error("boom");
  });
  EXPECT_EQ(guarded(), Action::kContinue);
}

}  // namespace
}  // namespace einheit::cli
