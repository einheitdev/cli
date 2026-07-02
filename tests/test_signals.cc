/// @file test_signals.cc
/// @brief Tests for the industrial signal regime — fault diagnosis,
/// SIGPIPE immunity, and the control-signal listener.
///
/// The fault-handler tests fork a child, install the handler, trigger a
/// real fault, and assert from the parent that (a) the child died from
/// the signal (so a core dump / WIFSIGNALED status is preserved for the
/// supervisor) and (b) the crash log names the signal and the last
/// command — a crash is always diagnosed, never silent.
// Copyright (c) 2026 Einheit Networks

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "einheit/cli/signals.h"

namespace einheit::cli::signals {
namespace {

auto TempPath(const std::string &tag) -> std::string {
  return "/tmp/einheit_sig_" + std::to_string(::getpid()) + "_" + tag;
}

auto ReadFile(const std::string &path) -> std::string {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

TEST(Signals, LastCommandRoundTripsAndTruncates) {
  SetLastCommand("show tunnels");
  EXPECT_EQ(LastCommand(), "show tunnels");

  const std::string big(2000, 'x');
  SetLastCommand(big);
  EXPECT_LT(LastCommand().size(), big.size());
  EXPECT_FALSE(LastCommand().empty());
}

// A real SIGSEGV in a forked child: the handler must log a diagnostic
// naming the signal + last command + a backtrace, then re-raise so the
// child dies from the signal (not exit()).
TEST(Signals, FaultHandlerLogsAndReRaises) {
  const std::string log = TempPath("crash");
  ::unlink(log.c_str());

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // Child: keep the console clean by muting stderr; the crash log
    // fd is separate.
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) ::dup2(devnull, STDERR_FILENO);
    InstallFaultHandlers(log);
    SetLastCommand("set i");
    // Trigger a genuine null-pointer write — the s5 crash class.
    volatile int *p = nullptr;
    *p = 1;
    ::_exit(0);  // unreachable
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  // Died from a signal (re-raised for a core dump), not a clean exit.
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);

  const std::string contents = ReadFile(log);
  ::unlink(log.c_str());
  EXPECT_NE(contents.find("SIGSEGV"), std::string::npos) << contents;
  EXPECT_NE(contents.find("set i"), std::string::npos) << contents;
  EXPECT_NE(contents.find("backtrace"), std::string::npos)
      << contents;
}

// SIGABRT (assert / std::terminate) is diagnosed the same way.
TEST(Signals, FaultHandlerCatchesAbort) {
  const std::string log = TempPath("abrt");
  ::unlink(log.c_str());
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) ::dup2(devnull, STDERR_FILENO);
    InstallFaultHandlers(log);
    SetLastCommand("commit");
    ::abort();
    ::_exit(0);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGABRT);
  const std::string contents = ReadFile(log);
  ::unlink(log.c_str());
  EXPECT_NE(contents.find("SIGABRT"), std::string::npos) << contents;
  EXPECT_NE(contents.find("commit"), std::string::npos) << contents;
}

// SIGPIPE must not kill the process once ignored: a peer disconnecting
// mid-write is a routine event, not a crash.
TEST(Signals, SigpipeIgnoredKeepsProcessAlive) {
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    IgnoreSigpipe();
    // Write to a pipe whose read end is closed → EPIPE + SIGPIPE.
    int fds[2];
    if (::pipe(fds) != 0) ::_exit(2);
    ::close(fds[0]);
    const char b = 'x';
    const ssize_t n = ::write(fds[1], &b, 1);
    // With SIGPIPE ignored, write returns -1/EPIPE instead of killing
    // us. Either way, reaching here means we survived.
    (void)n;
    ::_exit(7);
  }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status))
      << "process was killed by a signal (SIGPIPE not ignored)";
  EXPECT_EQ(WEXITSTATUS(status), 7);
}

// The control listener drains async signals from a signalfd and
// dispatches them to callbacks on its own thread.
TEST(Signals, ControlListenerDispatchesUsr2AndTerm) {
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<int> dumps{0};
  std::atomic<int> shutdowns{0};

  ControlHandlers h;
  h.on_dump_status = [&] {
    dumps.fetch_add(1);
    cv.notify_all();
  };
  h.on_shutdown = [&] {
    shutdowns.fetch_add(1);
    cv.notify_all();
  };

  {
    ControlListener listener(std::move(h));
    ASSERT_TRUE(listener.Running());

    // Process-directed (like a real SIGTERM from systemd or Ctrl-C to
    // the foreground group) so the signalfd on the listener thread
    // consumes it. A thread-directed raise() would stay pending on the
    // main thread instead.
    ::kill(::getpid(), SIGUSR2);
    {
      std::unique_lock<std::mutex> lk(mu);
      EXPECT_TRUE(cv.wait_for(lk, std::chrono::seconds(2),
                              [&] { return dumps.load() > 0; }));
    }

    ::kill(::getpid(), SIGTERM);
    {
      std::unique_lock<std::mutex> lk(mu);
      EXPECT_TRUE(cv.wait_for(lk, std::chrono::seconds(2),
                              [&] { return shutdowns.load() > 0; }));
    }
  }
  EXPECT_GE(dumps.load(), 1);
  EXPECT_GE(shutdowns.load(), 1);
}

}  // namespace
}  // namespace einheit::cli::signals
