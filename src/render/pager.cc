/// @file pager.cc
/// @brief Auto-pager implementation.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/pager.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace einheit::cli::render {
namespace {

auto CountLines(const std::string &s) -> std::size_t {
  std::size_t n = 0;
  for (const char c : s) {
    if (c == '\n') ++n;
  }
  return n;
}

}  // namespace

auto ShouldPage(const std::string &content, const TerminalCaps &caps)
    -> bool {
  if (!caps.is_tty) return false;
  if (caps.force_plain) return false;
  const auto lines = CountLines(content);
  // Leave room for the prompt + one line of context.
  return lines > static_cast<std::size_t>(caps.height) - 2;
}

auto Flush(const std::string &content, const TerminalCaps &caps)
    -> void {
  if (!ShouldPage(content, caps)) {
    std::cout << content;
    return;
  }
  const char *env = std::getenv("PAGER");
  const std::string cmd = (env && *env) ? env : "less -R";
  FILE *p = ::popen(cmd.c_str(), "w");
  if (!p) {
    std::cout << content;
    return;
  }
  std::fwrite(content.data(), 1, content.size(), p);
  ::pclose(p);
}

}  // namespace einheit::cli::render
