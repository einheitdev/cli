/// @file line_reader.h
/// @brief Abstraction over interactive line input.
///
/// Two implementations exist behind this interface: one backed by
/// GNU readline (tab completion, Ctrl-R, line editing) and a
/// fallback backed by `std::getline`. The fallback is always
/// available; the readline backend is compiled in when
/// `EINHEIT_HAVE_READLINE` is defined.
///
/// Completion candidates are supplied through a caller-provided
/// callback rather than global state, so the same reader can be
/// reused across different shell contexts.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_LINE_READER_H_
#define INCLUDE_EINHEIT_CLI_LINE_READER_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace einheit::cli {

/// Called on tab-complete with the tokens already entered and the
/// partial token being typed. Returns the candidate list.
using CompletionFn = std::function<std::vector<std::string>(
    const std::vector<std::string> &preceding,
    const std::string &partial)>;

/// One line of inline help: a candidate next token and an optional
/// one-line description. `help` may be empty.
struct HelpCandidate {
  std::string name;
  std::string help;
};

/// Called when the user presses `?` at any point in the line.
/// Returns the set of candidate next tokens (with help) for the
/// current cursor position. Unlike CompletionFn it includes help
/// strings so readers can render Junos-style inline help.
using HelpFn = std::function<std::vector<HelpCandidate>(
    const std::vector<std::string> &preceding,
    const std::string &partial)>;

/// Abstract reader. Owns prompt + history + completion + help
/// behaviour.
class LineReader {
 public:
  virtual ~LineReader() = default;

  /// Read one line. Returns nullopt on EOF.
  virtual auto ReadLine(const std::string &prompt)
      -> std::optional<std::string> = 0;

  /// Record a line in history. No-op for the fallback reader when
  /// history isn't persisted.
  virtual auto AddHistory(const std::string &line) -> void = 0;

  /// Install or replace the tab-completion callback.
  virtual auto SetCompletion(CompletionFn fn) -> void = 0;

  /// Install the `?` inline-help callback. No-op for readers that
  /// can't bind individual keys.
  virtual auto SetHelp(HelpFn fn) -> void = 0;
};

/// Construct the best available line reader. Prefers readline when
/// compiled in; otherwise returns a fallback that uses `std::cin`
/// with no line editing.
/// @returns Owned LineReader.
auto NewLineReader() -> std::unique_ptr<LineReader>;

}  // namespace einheit::cli

#endif  // INCLUDE_EINHEIT_CLI_LINE_READER_H_
