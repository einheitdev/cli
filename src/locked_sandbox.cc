/// @file locked_sandbox.cc
/// @brief seccomp-bpf install for --locked mode.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/locked_sandbox.h"

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace einheit::cli {
namespace {

auto MakeError(LockedSandboxError code, std::string msg)
    -> Error<LockedSandboxError> {
  return Error<LockedSandboxError>{code, std::move(msg)};
}

}  // namespace

auto InstallLockedSeccomp()
    -> std::expected<void, Error<LockedSandboxError>> {
  // Required by the kernel before SECCOMP_SET_MODE_FILTER unless
  // the caller has CAP_SYS_ADMIN. We never want CAP_SYS_ADMIN in
  // a cli, so always set NNP.
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    return std::unexpected(MakeError(
        LockedSandboxError::NoNewPrivsFailed,
        std::strerror(errno)));
  }

  std::vector<sock_filter> prog;
  // Validate arch: anything but x86_64 → kill the process. The
  // launcher and cli are built for the same arch in the same
  // tree, so a mismatch here only happens under weird cross-
  // exec conditions that we treat as a bug.
  prog.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                          offsetof(struct seccomp_data, arch)));
  prog.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                          AUDIT_ARCH_X86_64, 1, 0));
  prog.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));

  // Load syscall number.
  prog.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                          offsetof(struct seccomp_data, nr)));

  constexpr std::uint32_t kPermErrno =
      SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);

  // Deny execve (59) and execveat (322). EPERM rather than
  // SIGSYS so callers see a regular errno and can render a clean
  // error in the REPL.
  prog.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 59u, 0, 1));
  prog.push_back(BPF_STMT(BPF_RET | BPF_K, kPermErrno));
  prog.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 322u, 0, 1));
  prog.push_back(BPF_STMT(BPF_RET | BPF_K, kPermErrno));

  // Default: allow.
  prog.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));

  struct sock_fprog fprog {};
  fprog.len = static_cast<unsigned short>(prog.size());
  fprog.filter = prog.data();
  if (::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0u,
                &fprog) != 0) {
    return std::unexpected(MakeError(
        LockedSandboxError::SeccompFailed,
        std::strerror(errno)));
  }
  return {};
}

}  // namespace einheit::cli
