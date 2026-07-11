/// @file config_tree.cc
/// @brief Hierarchical rendering of flat config maps.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/render/config_tree.h"

#include <format>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "einheit/cli/render/theme.h"

namespace einheit::cli::render {
namespace {

struct TreeNode {
  // std::map keeps siblings alphabetical — deterministic output
  // from an unordered wire map.
  std::map<std::string, TreeNode> kids;
  bool is_leaf = false;
  char marker = 0;
  std::string value;
};

auto Insert(TreeNode &root, char marker, const std::string &path,
            std::string value) -> void {
  TreeNode *node = &root;
  std::string segment;
  std::istringstream iss(path);
  while (std::getline(iss, segment, '.')) {
    node = &node->kids[segment];
  }
  node->is_leaf = true;
  node->marker = marker;
  node->value = std::move(value);
}

/// Per-marker ANSI colour (empty when colour is off / no marker).
auto MarkerColor(char marker, const Theme &theme, bool color_ok)
    -> std::string {
  if (!color_ok) return "";
  switch (marker) {
    case '+': return FgAnsi(theme.good);
    case '-': return FgAnsi(theme.bad);
    case '~': return FgAnsi(theme.warn);
    case '=': return FgAnsi(theme.dim);
    default:  return "";
  }
}

auto Walk(const TreeNode &node, int depth, std::ostream &out,
          const Theme &theme, bool color_ok) -> void {
  constexpr const char *kReset = "\x1b[0m";
  const std::string bold = color_ok ? "\x1b[1m" : "";
  const std::string unbold = color_ok ? "\x1b[22m" : "";
  for (const auto &[name, child] : node.kids) {
    // The two leading columns are the diff gutter; indentation
    // starts after it so markers line up regardless of depth.
    const std::string indent(
        static_cast<std::size_t>(depth) * 2, ' ');
    if (child.is_leaf) {
      const std::string color =
          MarkerColor(child.marker, theme, color_ok);
      const std::string gutter =
          child.marker != 0 ? std::string(1, child.marker) + " "
                             : "  ";
      out << color << gutter << indent << name << ": "
          << child.value << (color.empty() ? "" : kReset) << "\n";
    }
    if (!child.kids.empty()) {
      out << "  " << indent << bold << name << unbold << " {\n";
      Walk(child, depth + 1, out, theme, color_ok);
      out << "  " << indent << "}\n";
    }
  }
}

}  // namespace

auto RenderConfigTree(const std::string &body, Renderer &renderer)
    -> void {
  if (renderer.Format() != OutputFormat::Table) {
    // Machine formats get the flat wire text — layout is for eyes.
    renderer.Out() << body;
    return;
  }

  TreeNode root;
  std::vector<std::string> loose;
  std::istringstream iss(body);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    char marker = 0;
    std::string rest = line;
    if (rest[0] == '+' || rest[0] == '-' || rest[0] == '~' ||
        rest[0] == '=') {
      marker = rest[0];
      rest.erase(0, 1);
    }
    const auto eq = rest.find('=');
    if (eq == std::string::npos) {
      loose.push_back(line);
      continue;
    }
    Insert(root, marker, rest.substr(0, eq), rest.substr(eq + 1));
  }

  const auto &caps = renderer.Caps();
  const bool color_ok =
      !caps.force_plain && caps.colors != ColorDepth::None;
  for (const auto &l : loose) {
    renderer.Out() << "  " << l << "\n";
  }
  Walk(root, 0, renderer.Out(), renderer.GetTheme(), color_ok);
}

}  // namespace einheit::cli::render
