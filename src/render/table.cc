/// @file table.cc
/// @brief Table rendering backed by FTXUI, with capability-aware
/// fallback and width-aware column dropping.
///
/// Behaviour per spec:
///  * Never wraps cells across lines.
///  * Drops lowest-priority columns first when the terminal is too
///    narrow to fit everything.
///  * Degrades to ASCII box-drawing + plain markers when caps can't
///    handle unicode + colour.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

namespace einheit::cli::render {
namespace {

auto PlainMarker(Semantic s) -> std::string {
  switch (s) {
    case Semantic::Good: return "[OK]";
    case Semantic::Warn: return "[WARN]";
    case Semantic::Bad:  return "[FAIL]";
    case Semantic::Dim:  return "[--]";
    default:             return "";
  }
}

auto DisplayText(const Cell &c, const TerminalCaps &caps)
    -> std::string {
  if (!caps.force_plain) return c.text;
  const auto marker = PlainMarker(c.semantic);
  return marker.empty() ? c.text : marker + " " + c.text;
}

auto ColorFor(Semantic s) -> ftxui::Color {
  using ftxui::Color;
  switch (s) {
    case Semantic::Good:     return Color::GreenLight;
    case Semantic::Warn:     return Color::YellowLight;
    case Semantic::Bad:      return Color::RedLight;
    case Semantic::Dim:      return Color::GrayDark;
    case Semantic::Emphasis: return Color::White;
    case Semantic::Info:     return Color::CyanLight;
    case Semantic::Default:
    default:                 return Color::Default;
  }
}

auto CellWidth(const Cell &c, const TerminalCaps &caps)
    -> std::size_t {
  return DisplayText(c, caps).size();
}

auto ColumnWidthsFor(const Table &t, const TerminalCaps &caps,
                     const std::vector<std::size_t> &visible)
    -> std::vector<std::size_t> {
  std::vector<std::size_t> widths(visible.size(), 0);
  for (std::size_t i = 0; i < visible.size(); ++i) {
    widths[i] = t.columns[visible[i]].header.size();
  }
  for (const auto &row : t.rows) {
    for (std::size_t i = 0; i < visible.size(); ++i) {
      const std::size_t src = visible[i];
      if (src >= row.size()) continue;
      widths[i] = std::max(widths[i], CellWidth(row[src], caps));
    }
  }
  return widths;
}

// Each column contributes content + 3 cells of border / separator
// overhead (left border, right border, vertical separator counted
// against the next column). A small constant close enough for the
// dropping heuristic.
constexpr std::size_t kBorderOverheadPerColumn = 3;

auto TotalRenderedWidth(const std::vector<std::size_t> &widths)
    -> std::size_t {
  std::size_t sum = 0;
  for (auto w : widths) sum += w + kBorderOverheadPerColumn;
  return sum;
}

// Determine which columns remain visible given the terminal width.
// Drops lowest-priority columns first; ties broken by rightmost.
auto ChooseVisible(const Table &t, const TerminalCaps &caps,
                   std::size_t budget)
    -> std::vector<std::size_t> {
  std::vector<std::size_t> visible(t.columns.size());
  for (std::size_t i = 0; i < t.columns.size(); ++i) visible[i] = i;

  while (visible.size() > 1) {
    auto widths = ColumnWidthsFor(t, caps, visible);
    if (TotalRenderedWidth(widths) <= budget) return visible;

    std::size_t drop_idx = 0;
    Priority worst = t.columns[visible[0]].priority;
    for (std::size_t i = 0; i < visible.size(); ++i) {
      const auto p = t.columns[visible[i]].priority;
      if (static_cast<int>(p) <= static_cast<int>(worst)) {
        worst = p;
        drop_idx = i;
      }
    }
    if (worst == Priority::High) return visible;
    visible.erase(visible.begin() + drop_idx);
  }
  return visible;
}

// One-space left + right padding inside every cell so tight content
// doesn't butt up against the column separators.
auto AlignedElement(const std::string &text, Align align)
    -> ftxui::Element {
  using namespace ftxui;
  Element e = ftxui::text(text);
  Element padded;
  switch (align) {
    case Align::Left:
      padded = hbox({ftxui::text(" "), e, filler(), ftxui::text(" ")});
      break;
    case Align::Right:
      padded = hbox({ftxui::text(" "), filler(), e, ftxui::text(" ")});
      break;
    case Align::Center:
      padded =
          hbox({ftxui::text(" "), filler(), e, filler(),
                ftxui::text(" ")});
      break;
  }
  return padded;
}

auto BuildFtxuiTable(const Table &t, const TerminalCaps &caps,
                     const std::vector<std::size_t> &visible)
    -> ftxui::Element {
  using namespace ftxui;
  std::vector<std::vector<Element>> grid;

  // Header row.
  std::vector<Element> header;
  header.reserve(visible.size());
  for (auto i : visible) {
    header.push_back(AlignedElement(t.columns[i].header,
                                    t.columns[i].align) |
                     bold);
  }
  grid.push_back(std::move(header));

  // Body rows.
  for (const auto &row : t.rows) {
    std::vector<Element> ftx_row;
    ftx_row.reserve(visible.size());
    for (auto i : visible) {
      const Cell cell = (i < row.size()) ? row[i] : Cell{};
      const auto &col = t.columns[i];
      const Align align = cell.align.value_or(col.align);
      const auto display = DisplayText(cell, caps);
      Element e = AlignedElement(display, align);
      if (!caps.force_plain &&
          caps.colors != ColorDepth::None &&
          cell.semantic != Semantic::Default) {
        e = e | color(ColorFor(cell.semantic));
      }
      if (cell.semantic == Semantic::Emphasis) e = e | bold;
      ftx_row.push_back(std::move(e));
    }
    grid.push_back(std::move(ftx_row));
  }

  ftxui::Table ftx(grid);
  const auto border_style = caps.unicode ? LIGHT : EMPTY;
  ftx.SelectAll().Border(border_style);
  ftx.SelectAll().SeparatorVertical(border_style);
  ftx.SelectRow(0).Decorate(bold);
  ftx.SelectRow(0).SeparatorHorizontal(border_style);
  return ftx.Render();
}

auto RenderViaFtxui(const Table &t, std::ostream &out,
                    const TerminalCaps &caps) -> void {
  using namespace ftxui;
  if (t.columns.empty()) return;

  const std::size_t budget = caps.width > 0 ? caps.width : 80;
  auto visible = ChooseVisible(t, caps, budget);

  auto element = BuildFtxuiTable(t, caps, visible);
  auto screen = Screen::Create(Dimension::Fit(element),
                               Dimension::Fit(element));
  Render(screen, element);
  out << screen.ToString() << '\n';
}

}  // namespace

auto AddColumn(Table &t, std::string header, Align align,
               Priority priority) -> void {
  Column c;
  c.header = std::move(header);
  c.align = align;
  c.priority = priority;
  t.columns.push_back(std::move(c));
}

auto AddRow(Table &t, std::vector<Cell> row) -> void {
  t.rows.push_back(std::move(row));
}

auto Render(const Table &t, std::ostream &out,
            const TerminalCaps &caps) -> void {
  RenderViaFtxui(t, out, caps);
}

namespace {

auto JsonEscape(const std::string &s) -> std::string {
  std::string out;
  out.reserve(s.size() + 2);
  for (const char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += std::format("\\u{:04x}",
                             static_cast<unsigned>(c));
        } else {
          out += c;
        }
    }
  }
  return out;
}

auto RenderJson(const Table &t, std::ostream &out) -> void {
  out << '[';
  for (std::size_t r = 0; r < t.rows.size(); ++r) {
    if (r > 0) out << ',';
    out << '{';
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
      if (c > 0) out << ',';
      const std::string &cell_text =
          (c < t.rows[r].size()) ? t.rows[r][c].text : std::string();
      out << '"' << JsonEscape(t.columns[c].header) << "\":\""
          << JsonEscape(cell_text) << '"';
    }
    out << '}';
  }
  out << "]\n";
}

auto RenderYaml(const Table &t, std::ostream &out) -> void {
  if (t.rows.empty()) {
    out << "[]\n";
    return;
  }
  for (const auto &row : t.rows) {
    bool first_field = true;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
      const std::string &cell_text =
          (c < row.size()) ? row[c].text : std::string();
      out << (first_field ? "- " : "  ")
          << t.columns[c].header << ": " << cell_text << '\n';
      first_field = false;
    }
  }
}

auto RenderSet(const Table &t, std::ostream &out) -> void {
  // Junos-ish recreation. First column is the anchor; later columns
  // emit `set <anchor> <field> <value>`.
  if (t.columns.size() < 2) return;
  for (const auto &row : t.rows) {
    if (row.empty()) continue;
    const std::string &anchor = row[0].text;
    for (std::size_t c = 1; c < t.columns.size(); ++c) {
      const std::string &cell_text =
          (c < row.size()) ? row[c].text : std::string();
      out << std::format("set {} {} {}\n", anchor,
                         t.columns[c].header, cell_text);
    }
  }
}

}  // namespace

auto RenderError(const std::string &code, const std::string &message,
                 const std::string &hint, Renderer &renderer)
    -> void {
  using namespace ftxui;
  const auto &caps = renderer.Caps();
  const bool colorful =
      !caps.force_plain && caps.colors != ColorDepth::None;

  if (!caps.unicode) {
    renderer.Out() << std::format("[FAIL] {}: {}\n", code, message);
    if (!hint.empty()) {
      renderer.Out() << std::format("       hint: {}\n", hint);
    }
    return;
  }

  std::vector<Element> lines;
  lines.push_back(hbox({
      text("error  ") | bold |
          (colorful ? color(Color::RedLight) : nothing),
      text(code) | bold,
  }));
  lines.push_back(text(message) |
                  (colorful ? color(Color::RedLight) : nothing));
  if (!hint.empty()) {
    lines.push_back(text(""));
    lines.push_back(
        hbox({text("hint: ") | bold |
                  (colorful ? color(Color::YellowLight) : nothing),
              text(hint) |
                  (colorful ? color(Color::YellowLight) : nothing)}));
  }

  Element body = vbox(std::move(lines));
  Element framed =
      body | borderRounded |
      (colorful ? color(Color::RedLight) : nothing);
  auto screen = Screen::Create(Dimension::Fit(framed),
                               Dimension::Fit(framed));
  Render(screen, framed);
  renderer.Out() << screen.ToString() << '\n';
}

auto RenderFormatted(const Table &t, Renderer &renderer) -> void {
  switch (renderer.Format()) {
    case OutputFormat::Json:
      RenderJson(t, renderer.Out());
      return;
    case OutputFormat::Yaml:
      RenderYaml(t, renderer.Out());
      return;
    case OutputFormat::Set:
      RenderSet(t, renderer.Out());
      return;
    case OutputFormat::Table:
    default:
      Render(t, renderer.Out(), renderer.Caps());
      return;
  }
}

}  // namespace einheit::cli::render
