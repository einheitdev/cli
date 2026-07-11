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
    // Enter with a menu on screen: wipe the grid first so command
    // output doesn't land on top of stale candidates.
    rx_.bind_key(replxx::Replxx::KEY::ENTER, NoThrow(
        [this](char32_t code) -> replxx::Replxx::ACTION_RESULT {
          ClearMenu();
          return rx_.invoke(
              replxx::Replxx::ACTION::COMMIT_LINE, code);
        }));
    // The grid must be painted AFTER replxx's own post-key refresh
    // (whose erase-below would wipe it). MenuStep schedules this
    // synthetic key; its handler runs on the next loop iteration,
    // once the line has been redrawn, and paints without dirtying
    // the state so no further refresh follows.
    rx_.bind_key(kMenuPaintKey, NoThrow(
        [this](char32_t) -> replxx::Replxx::ACTION_RESULT {
          if (menu_active_) RenderMenu();
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
  /// Synthetic key used to sequence grid painting after replxx's
  /// own refresh — a private-use codepoint no terminal emits.
  static constexpr char32_t kMenuPaintKey = 0x10FFFD;

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
      // Dotted config paths complete segment by segment: the
      // provider consumes the full dotted partial but returns
      // candidates for its LAST segment, so only that segment is
      // replaced. Plain command words have no dot — the segment
      // is the whole token.
      const auto token_start = before.size() - partial.size();
      const auto last_dot = partial.find_last_of('.');
      const auto seg_off =
          last_dot == std::string::npos ? 0 : last_dot + 1;
      const std::string segment = partial.substr(seg_off);
      const auto replace_start = token_start + seg_off;
      if (cands.size() == 1) {
        // A unique match completes outright — WITH a trailing
        // space, so the next TAB moves on to the next token
        // instead of re-completing this one (`show<TAB>` must not
        // need a hand-typed space to continue). A container path
        // ("dns.") keeps completing in place, and an existing
        // space after the cursor is not doubled.
        std::string token = cands.front();
        const bool mid_path = !token.empty() && token.back() == '.';
        const bool space_follows =
            cursor < text.size() && text[cursor] == ' ';
        if (!mid_path && !space_follows) token += ' ';
        ReplaceToken(text, replace_start, cursor, token);
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
      menu_token_start_ = replace_start;
      menu_rows_ = 0;
      menu_active_ = true;
      if (lcp.size() > segment.size()) {
        // The prefix grew — land there first, list the options,
        // and leave selection to the next press (zsh AUTO_MENU).
        menu_base_ = lcp;
        ReplaceToken(text, replace_start, cursor, lcp);
        rx_.emulate_key_press(kMenuPaintKey);
        return;
      }
      menu_base_ = segment;
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
    rx_.emulate_key_press(kMenuPaintKey);
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

  /// Print the candidate grid BELOW the input line — zsh-style:
  /// the prompt stays put — highlighting the selection (reverse
  /// video; a `>` gutter when colour is off). Draws by dropping
  /// under the prompt line and climbing back, so replxx repaints
  /// the prompt exactly where it was; relative moves keep this
  /// correct even when drawing at the bottom of the screen
  /// scrolls it.
  auto RenderMenu() -> void {
    int width = 80;
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0) {
      width = ws.ws_col;
    }
    // Container path candidates carry a functional trailing dot
    // ("dns." descends on the next pass); the grid shows the bare
    // name.
    const auto display = [](const std::string &item)
        -> std::string {
      if (!item.empty() && item.back() == '.') {
        return item.substr(0, item.size() - 1);
      }
      return item;
    };
    std::size_t cell = 1;
    for (const auto &it : menu_items_) {
      cell = std::max(cell, display(it).size());
    }
    // One-char selection gutter + two-space separator per cell.
    const int per_row = std::max(
        1, (width - 1) / static_cast<int>(cell + 3));
    const int n = static_cast<int>(menu_items_.size());
    const int rows = (n + per_row - 1) / per_row;

    std::string block = "\r\n";
    for (int row = 0; row < rows; ++row) {
      block += "\x1b[K";
      for (int col = 0; col < per_row; ++col) {
        const int i = row * per_row + col;
        if (i >= n) break;
        const bool sel = i == menu_index_;
        const std::string item = display(menu_items_[i]);
        const std::string pad(cell - item.size() + 2, ' ');
        if (sel && !no_color_) {
          block += " \x1b[7m" + item + "\x1b[27m" + pad;
        } else {
          block += (sel ? ">" : " ") + item + pad;
        }
      }
      if (row + 1 < rows) block += "\r\n";
    }
    // Clear anything below (a previous, taller grid), then climb
    // back to the input line for replxx's in-place repaint.
    block += "\x1b[0J";
    block += std::format("\x1b[{}A", rows);
    menu_rows_ = rows;
    rx_.print("%s", block.c_str());
  }

  /// Wipe a visible menu grid from below the input line (used
  /// before the line is committed, so command output doesn't land
  /// on top of stale candidates).
  auto ClearMenu() -> void {
    if (menu_rows_ <= 0) return;
    rx_.print("%s", "\n\x1b[0J\x1b[1A");
    menu_rows_ = 0;
    menu_active_ = false;
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
