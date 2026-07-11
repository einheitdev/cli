/// @file terminal_caps.cc
/// @brief Terminal capability probing.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/terminal_caps.h"

#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/ioctl.h>
#include <unistd.h>

namespace einheit::cli::render {
namespace {

auto Getenv(const char *name) -> std::string {
  const char *v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

auto DetectColors(const std::string &term,
                  const std::string &colorterm) -> ColorDepth {
  if (term == "dumb" || term.empty()) return ColorDepth::None;
  if (colorterm == "truecolor" || colorterm == "24bit") {
    return ColorDepth::TrueColor;
  }
  if (term.find("truecolor") != std::string::npos ||
      term.find("direct") != std::string::npos) {
    return ColorDepth::TrueColor;
  }
  if (term.find("256color") != std::string::npos) {
    return ColorDepth::Ansi256;
  }
  return ColorDepth::Ansi16;
}

auto DetectUnicode() -> bool {
  const char *locale = std::setlocale(LC_CTYPE, "");
  if (locale) {
    std::string s(locale);
    if (s != "C" && s != "POSIX") {
      for (auto &c : s) c = static_cast<char>(std::tolower(c));
      return s.find("utf-8") != std::string::npos ||
             s.find("utf8") != std::string::npos;
    }
  }
  // Appliance images often ship no generated locales, so the env
  // locale resolves to plain "C" and the whole CLI degrades to
  // ASCII borders. glibc's built-in C.UTF-8 needs no locale-gen —
  // prefer it over ASCII when the env gives us nothing better.
  return std::setlocale(LC_CTYPE, "C.UTF-8") != nullptr;
}

}  // namespace

auto DetectTerminal() -> TerminalCaps {
  TerminalCaps caps;
  caps.is_tty = ::isatty(STDOUT_FILENO) != 0;

  const std::string no_color = Getenv("NO_COLOR");
  if (!no_color.empty() || !caps.is_tty) {
    caps.force_plain = true;
  }

  std::string term = Getenv("TERM");
  // A real tty with no TERM at all is an env that got lost on the
  // way (sudo scrub, exotic SSH client), not a glass terminal —
  // over SSH, ANSI is a safe floor. Export the assumption so the
  // line editor (which makes its own TERM check) agrees with the
  // caps; a literal TERM=dumb stays respected.
  if (caps.is_tty && term.empty()) {
    term = "xterm";
    ::setenv("TERM", term.c_str(), 0);
  }
  const std::string colorterm = Getenv("COLORTERM");
  caps.colors = caps.force_plain
                    ? ColorDepth::None
                    : DetectColors(term, colorterm);
  caps.unicode = DetectUnicode();

  struct winsize ws{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
      ws.ws_col > 0 && ws.ws_row > 0) {
    caps.width = ws.ws_col;
    caps.height = ws.ws_row;
  }
  return caps;
}

auto ApplyOverrides(TerminalCaps base, const CapOverrides &overrides)
    -> TerminalCaps {
  if (overrides.color == 0) {
    base.force_plain = true;
    base.colors = ColorDepth::None;
  } else if (overrides.color == 1) {
    base.force_plain = false;
    // When the user explicitly forces colour, pick the richest
    // depth the env hints at. COLORTERM=truecolor / 24bit is the
    // canonical signal on modern terminals.
    if (base.colors == ColorDepth::None) {
      const std::string colorterm = Getenv("COLORTERM");
      if (colorterm == "truecolor" || colorterm == "24bit") {
        base.colors = ColorDepth::TrueColor;
      } else {
        const std::string term = Getenv("TERM");
        if (term.find("256color") != std::string::npos) {
          base.colors = ColorDepth::Ansi256;
        } else {
          base.colors = ColorDepth::Ansi16;
        }
      }
    }
  }
  if (overrides.force_ascii) base.unicode = false;
  if (overrides.width > 0) base.width = overrides.width;
  return base;
}

}  // namespace einheit::cli::render
