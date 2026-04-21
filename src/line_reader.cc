/// @file line_reader.cc
/// @brief LineReader implementations: readline when available,
/// std::getline fallback otherwise.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/line_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef EINHEIT_HAVE_READLINE
#include <readline/history.h>
#include <readline/readline.h>
#endif

namespace einheit::cli {
namespace {

auto Tokenize(const std::string &line) -> std::vector<std::string> {
  std::istringstream iss(line);
  std::vector<std::string> out;
  for (std::string t; iss >> t;) out.push_back(std::move(t));
  return out;
}

// Fallback: std::getline. No line editing, no completion, no help.
class StdinReader : public LineReader {
 public:
  auto ReadLine(const std::string &prompt)
      -> std::optional<std::string> override {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
  }
  auto AddHistory(const std::string & /*line*/) -> void override {}
  auto SetCompletion(CompletionFn /*fn*/) -> void override {}
  auto SetHelp(HelpFn /*fn*/) -> void override {}
};

#ifdef EINHEIT_HAVE_READLINE

// readline's C API uses globals for completion context; we thread
// the active callback through a singleton. The LineReader owns the
// callback's lifetime.
class ReadlineReader : public LineReader {
 public:
  ReadlineReader() {
    rl_attempted_completion_function = &ReadlineReader::Attempt;
    rl_bind_key('?', &ReadlineReader::ShowHelpKey);
    Active() = this;
  }
  ~ReadlineReader() override {
    if (Active() == this) Active() = nullptr;
  }

  auto ReadLine(const std::string &prompt)
      -> std::optional<std::string> override {
    char *raw = ::readline(prompt.c_str());
    if (!raw) return std::nullopt;
    std::string out(raw);
    std::free(raw);
    return out;
  }

  auto AddHistory(const std::string &line) -> void override {
    if (!line.empty()) ::add_history(line.c_str());
  }

  auto SetCompletion(CompletionFn fn) -> void override {
    fn_ = std::move(fn);
  }

  auto SetHelp(HelpFn fn) -> void override {
    help_fn_ = std::move(fn);
  }

 private:
  static auto Active() -> ReadlineReader *& {
    static ReadlineReader *p = nullptr;
    return p;
  }

  static auto Attempt(const char *text, int start, int /*end*/)
      -> char ** {
    auto *self = Active();
    if (!self || !self->fn_) return nullptr;

    // Split the buffer up to `start` into already-typed tokens.
    const std::string before(rl_line_buffer,
                             rl_line_buffer + start);
    auto preceding = Tokenize(before);

    auto candidates = self->fn_(preceding, std::string(text));
    if (candidates.empty()) return nullptr;

    // readline expects an owned char**; entries and container are
    // freed by readline via std::free.
    auto **result = static_cast<char **>(
        std::malloc((candidates.size() + 1) * sizeof(char *)));
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      result[i] = ::strdup(candidates[i].c_str());
    }
    result[candidates.size()] = nullptr;
    // Tell readline not to append a space after single matches —
    // path tokens like "interfaces." are usually continued.
    rl_completion_append_character = '\0';
    return result;
  }

  // Invoked when the user presses `?` anywhere in the line. Splits
  // rl_line_buffer around the cursor, asks the HelpFn, prints the
  // candidates with their help text on a fresh line, then redraws
  // the prompt + current input so the user can keep typing.
  static auto ShowHelpKey(int /*count*/, int /*key*/) -> int {
    auto *self = Active();
    if (!self || !self->help_fn_) return 0;

    const std::string before(rl_line_buffer,
                             rl_line_buffer + rl_point);
    std::vector<std::string> preceding;
    std::string partial;
    {
      std::istringstream iss(before);
      std::string tok;
      while (iss >> tok) preceding.push_back(tok);
      // If the buffer ends in a space, treat the last token as
      // "accepted" (no partial); otherwise it's the partial.
      if (!before.empty() && !std::isspace(
                                 static_cast<unsigned char>(
                                     before.back())) &&
          !preceding.empty()) {
        partial = preceding.back();
        preceding.pop_back();
      }
    }
    auto candidates = self->help_fn_(preceding, partial);

    rl_crlf();
    if (candidates.empty()) {
      std::cout << "  (no candidates)\n";
    } else {
      std::size_t w = 4;
      for (const auto &c : candidates) {
        w = std::max(w, c.name.size());
      }
      for (const auto &c : candidates) {
        std::cout << "  " << std::left << std::setw(static_cast<int>(w))
                  << c.name << "   " << c.help << '\n';
      }
    }
    rl_forced_update_display();
    return 0;
  }

  CompletionFn fn_;
  HelpFn help_fn_;
};

#endif  // EINHEIT_HAVE_READLINE

}  // namespace

auto NewLineReader() -> std::unique_ptr<LineReader> {
#ifdef EINHEIT_HAVE_READLINE
  return std::make_unique<ReadlineReader>();
#else
  return std::make_unique<StdinReader>();
#endif
}

}  // namespace einheit::cli
