/// @file signals.cc
/// @brief Industrial signal regime implementation.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/signals.h"

#include <execinfo.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <mutex>
#include <thread>

namespace einheit::cli::signals {
namespace {

// ---- Last-command buffer (read from the async fault handler) --------

constexpr std::size_t kLastCommandMax = 512;
// Fixed storage so the fault handler never touches the allocator. The
// writer keeps it NUL-terminated; a torn read during a crash is
// acceptable (we are dying anyway).
char g_last_command[kLastCommandMax] = {0};
std::atomic<std::size_t> g_last_command_len{0};
std::mutex g_last_command_mu;
// Optional shared-memory mirror the supervisor reads after a crash.
std::atomic<char *> g_mirror_buf{nullptr};
std::atomic<std::size_t> g_mirror_cap{0};

// ---- Async-signal-safe write helpers --------------------------------

// write() wrapper that retries on EINTR / partial writes. Uses only
// async-signal-safe calls.
auto SafeWrite(int fd, const char *buf, std::size_t len) -> void {
  if (fd < 0) return;
  std::size_t off = 0;
  while (off < len) {
    const ssize_t n = ::write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    off += static_cast<std::size_t>(n);
  }
}

// Bounded, async-signal-safe strlen + write.
auto SafeWriteStr(int fd, const char *s) -> void {
  std::size_t n = 0;
  while (s[n] != '\0' && n < 4096) ++n;
  SafeWrite(fd, s, n);
}

// Write an unsigned value as hex ("0x..") without malloc/printf.
auto SafeWriteHex(int fd, std::uintptr_t v) -> void {
  char buf[2 + sizeof(v) * 2];
  buf[0] = '0';
  buf[1] = 'x';
  const char *digits = "0123456789abcdef";
  int pos = 2;
  bool started = false;
  for (int shift = static_cast<int>(sizeof(v) * 8) - 4; shift >= 0;
       shift -= 4) {
    const int nibble = static_cast<int>((v >> shift) & 0xf);
    if (nibble != 0) started = true;
    if (started || shift == 0) buf[pos++] = digits[nibble];
  }
  SafeWrite(fd, buf, static_cast<std::size_t>(pos));
}

auto FaultSignalName(int sig) -> const char * {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    default:      return "signal";
  }
}

// The pre-opened crash-log fd. -1 means "stderr only".
std::atomic<int> g_crash_fd{-1};
// Fault-handler alternate stack, kept alive for the process lifetime.
constexpr std::size_t kAltStackSize = 1 << 16;  // 64 KiB
char *g_alt_stack = nullptr;
std::atomic<bool> g_fault_installed{false};

// The synchronous fault handler. MUST be async-signal-safe: only
// write()/backtrace_symbols_fd()/raise() and reads of static buffers.
extern "C" void FaultHandler(int sig, siginfo_t *info, void * /*uctx*/) {
  const int fds[2] = {STDERR_FILENO, g_crash_fd.load()};
  for (const int fd : fds) {
    if (fd < 0) continue;
    SafeWriteStr(fd, "\n=== einheit CLI crash: ");
    SafeWriteStr(fd, FaultSignalName(sig));
    if (info != nullptr &&
        (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL)) {
      SafeWriteStr(fd, " at fault address ");
      SafeWriteHex(
          fd, reinterpret_cast<std::uintptr_t>(info->si_addr));
    }
    SafeWriteStr(fd, " ===\nlast command: ");
    const std::size_t len =
        g_last_command_len.load(std::memory_order_relaxed);
    if (len > 0) {
      SafeWrite(fd, g_last_command, len);
    } else {
      SafeWriteStr(fd, "(none)");
    }
    SafeWriteStr(fd, "\nbacktrace:\n");
    // backtrace_symbols_fd is async-signal-safe (writes via write(),
    // no malloc), unlike backtrace_symbols.
    void *frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, fd);
    SafeWriteStr(fd,
                 "=== re-raising for core dump; session will end ===\n");
  }

  // Restore the default disposition and re-raise so the kernel produces
  // a core dump and the exit status reflects the signal (WIFSIGNALED),
  // which the supervisor uses to report the crash.
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  ::sigemptyset(&dfl.sa_mask);
  ::sigaction(sig, &dfl, nullptr);
  ::raise(sig);
}

}  // namespace

auto SetLastCommand(std::string_view command) -> void {
  std::lock_guard<std::mutex> lk(g_last_command_mu);
  const std::size_t n =
      command.size() < kLastCommandMax - 1 ? command.size()
                                           : kLastCommandMax - 1;
  std::memcpy(g_last_command, command.data(), n);
  g_last_command[n] = '\0';
  g_last_command_len.store(n, std::memory_order_relaxed);
  // Mirror into the supervisor's shared page, if registered.
  if (char *m = g_mirror_buf.load(); m != nullptr) {
    const std::size_t cap = g_mirror_cap.load();
    if (cap > 0) {
      const std::size_t k = n < cap - 1 ? n : cap - 1;
      std::memcpy(m, command.data(), k);
      m[k] = '\0';
    }
  }
}

auto MirrorLastCommandTo(char *buf, std::size_t cap) -> void {
  g_mirror_cap.store(cap);
  g_mirror_buf.store(buf);
}

auto LastCommand() -> std::string {
  std::lock_guard<std::mutex> lk(g_last_command_mu);
  return std::string(g_last_command,
                     g_last_command_len.load(std::memory_order_relaxed));
}

auto InstallFaultHandlers(const std::string &log_path) -> bool {
  if (!log_path.empty()) {
    const int fd = ::open(log_path.c_str(),
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                          0600);
    if (fd >= 0) g_crash_fd.store(fd);
  }

  // Alternate stack so a stack-overflow SEGV can still run the handler.
  if (g_alt_stack == nullptr) {
    g_alt_stack = new (std::nothrow) char[kAltStackSize];
    if (g_alt_stack != nullptr) {
      stack_t ss{};
      ss.ss_sp = g_alt_stack;
      ss.ss_size = kAltStackSize;
      ss.ss_flags = 0;
      ::sigaltstack(&ss, nullptr);
    }
  }

  struct sigaction sa{};
  sa.sa_sigaction = &FaultHandler;
  ::sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  bool ok = true;
  for (const int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
    if (::sigaction(sig, &sa, nullptr) != 0) ok = false;
  }
  g_fault_installed.store(ok);
  return ok;
}

auto IgnoreSigpipe() -> void {
  struct sigaction sa{};
  sa.sa_handler = SIG_IGN;
  ::sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  ::sigaction(SIGPIPE, &sa, nullptr);
}

// ---- Control-signal listener (signalfd + dedicated thread) ----------

struct ControlListener::Impl {
  ControlHandlers handlers;
  int sfd = -1;
  // Self-pipe used to wake the poll loop on shutdown.
  int wake[2] = {-1, -1};
  std::thread thread;
  std::atomic<bool> running{false};

  void Run() {
    struct pollfd fds[2];
    fds[0].fd = sfd;
    fds[0].events = POLLIN;
    fds[1].fd = wake[0];
    fds[1].events = POLLIN;
    while (true) {
      const int rc = ::poll(fds, 2, -1);
      if (rc < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (fds[1].revents & POLLIN) break;  // stop requested
      if (!(fds[0].revents & POLLIN)) continue;
      struct signalfd_siginfo si;
      const ssize_t n = ::read(sfd, &si, sizeof(si));
      if (n != static_cast<ssize_t>(sizeof(si))) continue;
      switch (si.ssi_signo) {
        case SIGINT:
          if (handlers.on_interrupt) {
            handlers.on_interrupt();
            break;
          }
          [[fallthrough]];
        case SIGTERM:
        case SIGQUIT:
        case SIGHUP:
          if (handlers.on_shutdown) handlers.on_shutdown();
          break;
        case SIGUSR1:
          if (handlers.on_reopen_logs) handlers.on_reopen_logs();
          break;
        case SIGUSR2:
          if (handlers.on_dump_status) handlers.on_dump_status();
          break;
        default:
          break;
      }
    }
  }
};

ControlListener::ControlListener(ControlHandlers handlers)
    : impl_(std::make_unique<Impl>()) {
  impl_->handlers = std::move(handlers);

  sigset_t mask;
  ::sigemptyset(&mask);
  for (const int sig :
       {SIGTERM, SIGINT, SIGQUIT, SIGHUP, SIGUSR1, SIGUSR2}) {
    ::sigaddset(&mask, sig);
  }
  // Block in this thread; threads spawned afterwards inherit the block,
  // so the signals are delivered only via the signalfd, never to a
  // default disposition.
  if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) return;

  impl_->sfd = ::signalfd(-1, &mask, SFD_CLOEXEC);
  if (impl_->sfd < 0) return;
  if (::pipe(impl_->wake) != 0) {
    ::close(impl_->sfd);
    impl_->sfd = -1;
    return;
  }

  impl_->running.store(true);
  impl_->thread = std::thread([this] { impl_->Run(); });
}

ControlListener::~ControlListener() {
  if (!impl_) return;
  if (impl_->running.load() && impl_->wake[1] >= 0) {
    const char b = 1;
    ssize_t rc = ::write(impl_->wake[1], &b, 1);
    (void)rc;
  }
  if (impl_->thread.joinable()) impl_->thread.join();
  for (int fd : {impl_->sfd, impl_->wake[0], impl_->wake[1]}) {
    if (fd >= 0) ::close(fd);
  }
  // Restore the disposition: unblock the signals this listener owned.
  if (impl_->running.load()) {
    sigset_t mask;
    ::sigemptyset(&mask);
    for (const int sig :
         {SIGTERM, SIGINT, SIGQUIT, SIGHUP, SIGUSR1, SIGUSR2}) {
      ::sigaddset(&mask, sig);
    }
    ::pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
  }
}

auto ControlListener::Running() const -> bool {
  return impl_ && impl_->running.load();
}

}  // namespace einheit::cli::signals
