/// @file line_reader.cc
/// @brief LineReader backed by replxx, with a minimal std::cin
/// fallback for non-TTY input (pipes, --replay, tests).
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/line_reader.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <replxx.hxx>

namespace einheit::cli {
namespace {

auto Tokenize(const std::string &line)
    -> std::vector<std::string> {
  std::istringstream iss(line);
  std::vector<std::string> out;
  for (std::string t; iss >> t;) out.push_back(std::move(t));
  return out;
}

// Non-TTY fallback: replxx prints its prompt via stdout + raw
// terminal control, which is wrong when stdin is a pipe. The
// getline path just echoes the prompt and reads a line.
class StdinReader : public LineReader {
 public:
  auto ReadLine(const std::string &prompt)
      -> std::optional<std::string> override {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
  }
  auto AddHistory(const std::string &) -> void override {}
  auto SetCompletion(CompletionFn) -> void override {}
  auto SetHelp(HelpFn) -> void override {}
};

// replxx-backed reader. replxx exposes modern hooks we use:
//  * set_completion_callback — tab completion candidates
//  * set_hint_callback       — grey right-side "ghost" hints
//  * bind_key('?')           — inline help popup
class ReplxxReader : public LineReader {
 public:
  ReplxxReader() {
    rx_.set_word_break_characters(" \t");
    rx_.set_completion_callback(
        [this](const std::string &ctx, int &len)
            -> replxx::Replxx::completions_t {
          if (!completion_fn_) return {};
          const auto preceding = Tokenize(std::string(
              ctx.data(), ctx.data() + ctx.size() - len));
          const std::string partial(
              ctx.data() + ctx.size() - len, len);
          replxx::Replxx::completions_t out;
          for (const auto &cand :
               completion_fn_(preceding, partial)) {
            out.emplace_back(cand.c_str());
          }
          return out;
        });
    rx_.set_hint_callback(
        [this](const std::string &ctx, int &len,
               replxx::Replxx::Color &color)
            -> replxx::Replxx::hints_t {
          color = replxx::Replxx::Color::GRAY;
          if (!help_fn_) return {};
          // Empty buffer — don't hint. Otherwise the completion
          // set expands to every registered verb and we'd pick
          // an arbitrary one, which reads as noise.
          if (ctx.empty() && len == 0) return {};
          const auto preceding = Tokenize(std::string(
              ctx.data(), ctx.data() + ctx.size() - len));
          const std::string partial(
              ctx.data() + ctx.size() - len, len);
          // No partial at the cursor (trailing whitespace) —
          // user isn't asking for a specific completion yet.
          if (partial.empty()) return {};
          auto candidates = help_fn_(preceding, partial);
          if (candidates.empty()) return {};
          replxx::Replxx::hints_t out;
          out.emplace_back(candidates.front().name.c_str());
          return out;
        });
    // `?` mid-line opens an inline help popup via the HelpFn.
    rx_.bind_key('?',
                 [this](char32_t) -> replxx::Replxx::ACTION_RESULT {
                   ShowHelpOverlay();
                   return replxx::Replxx::ACTION_RESULT::CONTINUE;
                 });
  }

  auto ReadLine(const std::string &prompt)
      -> std::optional<std::string> override {
    const char *line = rx_.input(prompt.c_str());
    if (!line) return std::nullopt;
    return std::string(line);
  }

  auto AddHistory(const std::string &line) -> void override {
    if (!line.empty()) rx_.history_add(line);
  }

  auto SetCompletion(CompletionFn fn) -> void override {
    completion_fn_ = std::move(fn);
  }

  auto SetHelp(HelpFn fn) -> void override {
    help_fn_ = std::move(fn);
  }

 private:
  auto ShowHelpOverlay() -> void {
    if (!help_fn_) return;
    const std::string raw_ctx(rx_.get_state().text());
    const auto cursor =
        static_cast<std::size_t>(rx_.get_state().cursor_position());
    const std::string before = raw_ctx.substr(
        0, std::min(cursor, raw_ctx.size()));
    std::vector<std::string> preceding;
    std::string partial;
    {
      std::istringstream iss(before);
      std::string tok;
      while (iss >> tok) preceding.push_back(tok);
      if (!before.empty() &&
          !std::isspace(
              static_cast<unsigned char>(before.back())) &&
          !preceding.empty()) {
        partial = preceding.back();
        preceding.pop_back();
      }
    }
    auto candidates = help_fn_(preceding, partial);

    // Lead with CR + ESC[K so the overlay starts at column
    // 0 and the prompt line we were typing on gets cleared
    // before the block; replxx redraws the prompt + buffer
    // after we return, which otherwise starts wherever the
    // cursor happened to be and renders as an indented
    // orphan.
    std::string block;
    block += "\r\x1b[K\n";
    if (candidates.empty()) {
      block += "  (no candidates)\n";
    } else {
      std::size_t w = 4;
      for (const auto &c : candidates) {
        w = std::max(w, c.name.size());
      }
      for (const auto &c : candidates) {
        block += "  ";
        block += c.name;
        block += std::string(w - c.name.size() + 3, ' ');
        block += c.help;
        block += '\n';
      }
    }
    rx_.print("%s", block.c_str());
  }

  replxx::Replxx rx_;
  CompletionFn completion_fn_;
  HelpFn help_fn_;
};

}  // namespace

auto NewLineReader() -> std::unique_ptr<LineReader> {
  if (::isatty(STDIN_FILENO) == 0) {
    return std::make_unique<StdinReader>();
  }
  return std::make_unique<ReplxxReader>();
}

}  // namespace einheit::cli
