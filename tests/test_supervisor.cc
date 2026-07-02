/// @file test_supervisor.cc
/// @brief Tests for the crash supervisor: a child that dies from a
/// fault signal is reaped and reported cleanly (not a dead prompt); a
/// clean exit passes through; a termination signal is reported as a
/// stop, not a crash.
// Copyright (c) 2026 Einheit Networks

#include <csignal>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/signals.h"
#include "einheit/cli/supervisor.h"

namespace einheit::cli {
namespace {

TEST(Supervisor, CleanExitPassesThroughCode) {
  std::ostringstream out;
  SupervisorOptions opts;
  opts.out = &out;
  SupervisorResult r;
  const int rc = RunSupervised([] { return 3; }, opts, &r);
  EXPECT_EQ(rc, 3);
  EXPECT_EQ(r.outcome, ChildOutcome::Exited);
  EXPECT_EQ(r.exit_code, 3);
  // No crash notice on a clean exit.
  EXPECT_TRUE(out.str().empty()) << out.str();
}

// Detect AddressSanitizer, which intercepts SEGV and exits rather than
// letting the process die from the signal — so the authentic null-deref
// crash test can only assert a signal death outside ASan.
#if defined(__SANITIZE_ADDRESS__)
#define EINHEIT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define EINHEIT_ASAN 1
#endif
#endif

TEST(Supervisor, ChildCrashReportedWithLastCommand) {
  std::ostringstream out;
  SupervisorOptions opts;
  opts.out = &out;
  opts.crash_log_path = "/var/log/einheit/crash.log";
  SupervisorResult r;

  const int rc = RunSupervised(
      [] {
        // Record what we were "running", then take a fault signal.
        // abort() (SIGABRT) is used rather than a null deref so the
        // test is robust under ASan too; the SEGV path is covered by
        // ChildSegfaultIsACrash below (non-ASan builds).
        signals::SetLastCommand("set interfaces.eth0.ip");
        ::abort();
        return 0;  // unreachable
      },
      opts, &r);

  EXPECT_EQ(r.outcome, ChildOutcome::Crashed);
  EXPECT_TRUE(IsFaultSignal(r.signal));
  EXPECT_EQ(rc, 128 + r.signal);
  // The parent recovered the last command from shared memory and named
  // it in the notice, along with the log path.
  EXPECT_EQ(r.last_command, "set interfaces.eth0.ip");
  const std::string msg = out.str();
  EXPECT_NE(msg.find("crashed"), std::string::npos) << msg;
  EXPECT_NE(msg.find("set interfaces.eth0.ip"), std::string::npos)
      << msg;
  EXPECT_NE(msg.find("crash.log"), std::string::npos) << msg;
  // Never leaves the user staring at a dead prompt.
  EXPECT_NE(msg.find("dead prompt"), std::string::npos) << msg;
}

// Authentic null-pointer dereference — the s5 crash class — reaped and
// classified as a crash. Skipped under ASan (which converts the SEGV
// into a process exit).
TEST(Supervisor, ChildSegfaultIsACrash) {
#if defined(EINHEIT_ASAN)
  GTEST_SKIP() << "ASan intercepts SEGV; covered by Release build";
#else
  std::ostringstream out;
  SupervisorOptions opts;
  opts.out = &out;
  SupervisorResult r;
  const int rc = RunSupervised(
      [] {
        signals::SetLastCommand("set i");
        volatile int *p = nullptr;
        *p = 1;
        return 0;
      },
      opts, &r);
  EXPECT_EQ(r.outcome, ChildOutcome::Crashed);
  EXPECT_EQ(r.signal, SIGSEGV);
  EXPECT_EQ(rc, 128 + SIGSEGV);
  EXPECT_EQ(r.last_command, "set i");
#endif
}

TEST(Supervisor, ChildAbortIsACrash) {
  std::ostringstream out;
  SupervisorOptions opts;
  opts.out = &out;
  SupervisorResult r;
  const int rc = RunSupervised(
      [] {
        ::abort();
        return 0;
      },
      opts, &r);
  EXPECT_EQ(r.outcome, ChildOutcome::Crashed);
  EXPECT_EQ(r.signal, SIGABRT);
  EXPECT_EQ(rc, 128 + SIGABRT);
}

TEST(Supervisor, TerminationSignalIsNotACrash) {
  std::ostringstream out;
  SupervisorOptions opts;
  opts.out = &out;
  SupervisorResult r;
  // Child kills itself with SIGTERM — a graceful stop, not a crash.
  const int rc = RunSupervised(
      [] {
        ::raise(SIGTERM);
        return 0;
      },
      opts, &r);
  EXPECT_EQ(r.outcome, ChildOutcome::Terminated);
  EXPECT_EQ(r.signal, SIGTERM);
  EXPECT_EQ(rc, 128 + SIGTERM);
  const std::string msg = out.str();
  EXPECT_NE(msg.find("terminated"), std::string::npos) << msg;
  EXPECT_EQ(msg.find("crashed"), std::string::npos)
      << "a TERM must not be reported as a crash: " << msg;
}

TEST(Supervisor, IsFaultSignalClassification) {
  for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
    EXPECT_TRUE(IsFaultSignal(s)) << s;
  }
  for (int s : {SIGTERM, SIGINT, SIGQUIT, SIGHUP, SIGUSR1, SIGUSR2}) {
    EXPECT_FALSE(IsFaultSignal(s)) << s;
  }
}

}  // namespace
}  // namespace einheit::cli
