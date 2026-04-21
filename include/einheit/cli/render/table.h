/// @file table.h
/// @brief Semantic table primitives. Command handlers build a Table
/// from Cells; the renderer picks representation per TerminalCaps.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_TABLE_H_
#define INCLUDE_EINHEIT_CLI_RENDER_TABLE_H_

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "einheit/cli/render/terminal_caps.h"

namespace einheit::cli::render {

/// Colour-independent meaning of a cell.
enum class Semantic {
  Default,
  /// Green — healthy, active, allowed.
  Good,
  /// Yellow — degraded, connecting, rate-limited.
  Warn,
  /// Red — failed, denied, dropped.
  Bad,
  /// Grey — inactive, stale.
  Dim,
  /// White bold — headers, important values.
  Emphasis,
  /// Cyan — labels, neutral highlights.
  Info,
};

/// Horizontal alignment inside a cell.
enum class Align { Left, Center, Right };

/// Column priority; lowest drops first on narrow terminals.
enum class Priority { Low, Medium, High };

/// One table cell. `align` is optional — when unset the cell inherits
/// the containing column's alignment. Override only when a single
/// cell needs to differ from its column (e.g. a right-aligned total
/// row in a left-aligned column).
struct Cell {
  std::string text;
  Semantic semantic = Semantic::Default;
  std::optional<Align> align;
};

/// Column declaration. Data for rows lives separately.
struct Column {
  std::string header;
  Align align = Align::Left;
  Priority priority = Priority::Medium;
  /// Minimum content width before the column is dropped.
  std::uint16_t min_width = 1;
};

/// A table is columns + rows; data-oriented, no behaviour on struct.
struct Table {
  std::vector<Column> columns;
  std::vector<std::vector<Cell>> rows;
};

/// Append a column to the table.
/// @param t Table being built.
/// @param header Column header.
/// @param align Horizontal alignment.
/// @param priority Drop priority on narrow terminals.
auto AddColumn(Table &t, std::string header,
               Align align = Align::Left,
               Priority priority = Priority::Medium) -> void;

/// Append a row to the table. Cell count must equal column count.
/// @param t Table being built.
/// @param row Row data.
auto AddRow(Table &t, std::vector<Cell> row) -> void;

/// Render the table to an output stream according to caps. Always
/// produces the capability-aware table layout.
/// @param t The table.
/// @param out Destination stream (stdout or a string for tests).
/// @param caps Terminal capabilities.
auto Render(const Table &t, std::ostream &out,
            const TerminalCaps &caps) -> void;

class Renderer;

/// Format-aware table output. Picks Table / JSON / YAML / set-syntax
/// based on `renderer.Format()`. Writes to `renderer.Out()`. Cell
/// text carries through unmodified; cell colour/semantic is dropped
/// for non-Table formats (machine-readable output shouldn't carry
/// ANSI).
/// @param t The table.
/// @param renderer Renderer carrying out stream + caps + format.
auto RenderFormatted(const Table &t, Renderer &renderer) -> void;

/// Emit the ANSI sequence that clears the terminal and homes the
/// cursor. No-op when caps can't handle ANSI. Used by watch loops
/// to redraw in place instead of appending.
/// @param out Destination stream.
/// @param caps Terminal capabilities.
auto ClearScreen(std::ostream &out, const TerminalCaps &caps)
    -> void;

/// Render a daemon error as a bordered red FTXUI box showing code +
/// message + optional hint. Used by every adapter's RenderResponse
/// when Response::error is populated; keeps error styling consistent
/// across products.
/// @param code Machine-readable error code.
/// @param message Human-readable message.
/// @param hint Optional hint text; empty string omits the line.
/// @param renderer Destination.
auto RenderError(const std::string &code, const std::string &message,
                 const std::string &hint, Renderer &renderer)
    -> void;

/// Output serialisation the user asked for. Human-table is the
/// default; JSON / YAML / set-syntax are opt-in via --format.
enum class OutputFormat {
  /// Capability-aware table with colours when the terminal supports.
  Table,
  /// Compact JSON; suitable for `| jq`.
  Json,
  /// YAML; suitable for inspection + config round-trip.
  Yaml,
  /// Junos-style `set` commands that recreate the data.
  Set,
};

/// Renderer handle passed to adapters via ProductAdapter
/// render callbacks. Wraps stdout + TerminalCaps so individual
/// renderers don't have to thread those through themselves.
class Renderer {
 public:
  Renderer(std::ostream &out, TerminalCaps caps,
           OutputFormat format = OutputFormat::Table) noexcept
      : out_(&out), caps_(caps), format_(format) {}

  auto Out() -> std::ostream & { return *out_; }
  auto Caps() const -> const TerminalCaps & { return caps_; }
  auto Format() const -> OutputFormat { return format_; }

 private:
  std::ostream *out_;
  TerminalCaps caps_;
  OutputFormat format_;
};

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_TABLE_H_
