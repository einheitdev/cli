/// @file line_reader.cc
/// @brief LineReader backed by replxx, with a minimal std::cin
/// fallback for non-TTY input (pipes, --replay, tests).
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/line_reader.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <format>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <replxx.hxx>

#include "einheit/cli/no_throw.h"

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
  explicit ReplxxReader(bool no_color) : no_color_(no_color) {
    // replxx colours its completion menu and hints on its own,
    // bypassing the renderer's capability gate. When the caps say
    // plain (NO_COLOR, dumb pipe, ColorDepth::None), replxx must
    // go plain too or "no colour" terminals get raw SGR garbage.
    rx_.set_no_color(no_color);
    rx_.set_word_break_characters(" \t");
    // Every callback handed to replxx is wrapped in NoThrow at the
    // registration site (gap #7). An exception must NEVER cross back
    // into replxx's C input loop — it would std::terminate the whole
    // CLI. The guard is applied once, here, so a new callback can't
    // silently reintroduce the crash by forgetting a try/catch.
    rx_.set_completion_callback(NoThrow(
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
        }));
    rx_.set_hint_callback(NoThrow(
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
        }));
    // `?` mid-line opens an inline help popup via the HelpFn.
    rx_.bind_key('?', NoThrow(
        [this](char32_t) -> replxx::Replxx::ACTION_RESULT {
          ShowHelpOverlay();
          return replxx::Replxx::ACTION_RESULT::CONTINUE;
        }));
    // zsh-style menu completion: TAB extends the common prefix,
    // then lists the candidates below the line and each further
    // TAB steps the highlight through them, rewriting the token in
    // place (Shift-TAB steps back; one step past the end restores
    // the typed prefix).
    rx_.bind_key(replxx::Replxx::KEY::TAB, NoThrow(
        [this](char32_t) -> replxx::Replxx::ACTION_RESULT {
          MenuStep(+1);
          return replxx::Replxx::ACTION_RESULT::CONTINUE;
        }));
    rx_.bind_key(
        replxx::Replxx::KEY::shift(replxx::Replxx::KEY::TAB),
        NoThrow([this](char32_t)
                    -> replxx::Replxx::ACTION_RESULT {
          MenuStep(-1);
          return replxx::Replxx::ACTION_RESULT::CONTINUE;
        }));
  }

  auto ReadLine(const std::string &prompt)
      -> std::optional<std::string> override {
    errno = 0;
    const char *line = rx_.input(prompt.c_str());
    if (!line) {
      // replxx returns nullptr for BOTH end-of-input (Ctrl-D) and
      // a cancelled line (Ctrl-C, errno == EAGAIN). Only the
      // former may end the shell — an operator's reflex Ctrl-C
      // over SSH must clear the line, never log the session out.
      if (errno == EAGAIN) return std::string();
      return std::nullopt;
    }
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
  /// One TAB / Shift-TAB press of the zsh-style completion menu.
  /// First press extends the common prefix and (when ambiguous)
  /// lists the candidates; further presses move the highlight and
  /// rewrite the token in place. dir is +1 for TAB, -1 for
  /// Shift-TAB; stepping past either end restores the typed
  /// prefix.
  auto MenuStep(int dir) -> void {
    if (!completion_fn_) return;
    const std::string text(rx_.get_state().text());
    const auto cursor = static_cast<std::size_t>(
        rx_.get_state().cursor_position());
    if (menu_active_ && (text != menu_expected_text_ ||
                         cursor != menu_expected_cursor_)) {
      // The operator edited the line since the menu went up.
      menu_active_ = false;
    }
    if (!menu_active_) {
      const std::string before =
          text.substr(0, std::min(cursor, text.size()));
      auto preceding = Tokenize(before);
      std::string partial;
      if (!before.empty() &&
          !std::isspace(
              static_cast<unsigned char>(before.back())) &&
          !preceding.empty()) {
        partial = preceding.back();
        preceding.pop_back();
      }
      auto cands = completion_fn_(preceding, partial);
      if (cands.empty()) return;
      const auto token_start = before.size() - partial.size();
      if (cands.size() == 1) {
        ReplaceToken(text, token_start, cursor, cands.front());
        menu_active_ = false;
        return;
      }
      std::string lcp = cands.front();
      for (const auto &c : cands) {
        std::size_t i = 0;
        while (i < lcp.size() && i < c.size() && lcp[i] == c[i]) {
          ++i;
        }
        lcp.resize(i);
      }
      menu_items_ = std::move(cands);
      menu_index_ = -1;
      menu_token_start_ = token_start;
      menu_rows_ = 0;
      menu_active_ = true;
      if (lcp.size() > partial.size()) {
        // The prefix grew — land there first, list the options,
        // and leave selection to the next press (zsh AUTO_MENU).
        menu_base_ = lcp;
        ReplaceToken(text, token_start, cursor, lcp);
        RenderMenu();
        return;
      }
      menu_base_ = partial;
      menu_expected_text_ = text;
      menu_expected_cursor_ = cursor;
    }
    const int n = static_cast<int>(menu_items_.size());
    menu_index_ += dir;
    if (menu_index_ >= n) {
      menu_index_ = -1;
    } else if (menu_index_ < -1) {
      menu_index_ = n - 1;
    }
    const std::string &token = menu_index_ == -1
                                   ? menu_base_
                                   : menu_items_[menu_index_];
    ReplaceToken(menu_expected_text_, menu_token_start_,
                 menu_expected_cursor_, token);
    RenderMenu();
  }

  /// Replace [start, end) of `text` with `token`, park the cursor
  /// after it, and remember the resulting state so a later TAB can
  /// tell whether the menu is still current.
  auto ReplaceToken(const std::string &text, std::size_t start,
                    std::size_t end, const std::string &token)
      -> void {
    const std::string next =
        text.substr(0, start) + token + text.substr(end);
    const auto pos = static_cast<int>(start + token.size());
    rx_.set_state(replxx::Replxx::State(next.c_str(), pos));
    menu_expected_text_ = next;
    menu_expected_cursor_ = start + token.size();
  }

  /// Print the candidate grid above the input line, highlighting
  /// the selection (reverse video; a `>` gutter when colour is
  /// off). Re-renders in place while cycling by climbing back over
  /// the previous grid.
  auto RenderMenu() -> void {
    int width = 80;
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0) {
      width = ws.ws_col;
    }
    std::size_t cell = 1;
    for (const auto &it : menu_items_) {
      cell = std::max(cell, it.size());
    }
    // One-char selection gutter + two-space separator per cell.
    const int per_row = std::max(
        1, (width - 1) / static_cast<int>(cell + 3));
    const int n = static_cast<int>(menu_items_.size());
    const int rows = (n + per_row - 1) / per_row;

    std::string block = "\r\x1b[K";
    if (menu_rows_ > 0) {
      // Overwrite the grid from the previous step instead of
      // scrolling a fresh copy in.
      block += std::format("\x1b[{}A", menu_rows_);
    }
    for (int row = 0; row < rows; ++row) {
      block += "\x1b[K";
      for (int col = 0; col < per_row; ++col) {
        const int i = row * per_row + col;
        if (i >= n) break;
        const bool sel = i == menu_index_;
        const auto &item = menu_items_[i];
        const std::string pad(cell - item.size() + 2, ' ');
        if (sel && !no_color_) {
          block += " \x1b[7m" + item + "\x1b[27m" + pad;
        } else {
          block += (sel ? ">" : " ") + item + pad;
        }
      }
      block += "\n";
    }
    menu_rows_ = rows;
    rx_.print("%s", block.c_str());
  }

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
  bool no_color_ = false;
  // zsh-style completion-menu state (see MenuStep).
  bool menu_active_ = false;
  std::vector<std::string> menu_items_;
  int menu_index_ = -1;
  std::string menu_base_;
  std::size_t menu_token_start_ = 0;
  std::string menu_expected_text_;
  std::size_t menu_expected_cursor_ = 0;
  int menu_rows_ = 0;
};

}  // namespace

auto NewLineReader(bool no_color) -> std::unique_ptr<LineReader> {
  if (::isatty(STDIN_FILENO) == 0) {
    return std::make_unique<StdinReader>();
  }
  return std::make_unique<ReplxxReader>(no_color);
}

}  // namespace einheit::cli
