/// @file config_tree.h
/// @brief Hierarchical (Junos-style) rendering of flat config maps.
///
/// confd's canonical config is a flat dotted-path map; this folds
/// its key=value wire encoding into an indented tree for display:
/// containers open `name {` blocks, leaves print `name: value`, and
/// the diff markers (+ added, ~ changed, - removed, = unchanged)
/// keep their per-line colouring in a fixed gutter column.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_RENDER_CONFIG_TREE_H_
#define INCLUDE_EINHEIT_CLI_RENDER_CONFIG_TREE_H_

#include <string>

#include "einheit/cli/render/table.h"

namespace einheit::cli::render {

/// Render a confd key=value body — plain (`show config`) or
/// diff-marked (`show diff`, `show commit`) — as an indented tree.
/// Paths sort alphabetically so output is deterministic regardless
/// of map order. Non-Table output formats receive the body
/// unmodified (machine-readable output carries no layout).
/// @param body The response data decoded as text.
/// @param renderer Output stream + caps + theme.
auto RenderConfigTree(const std::string &body, Renderer &renderer)
    -> void;

}  // namespace einheit::cli::render

#endif  // INCLUDE_EINHEIT_CLI_RENDER_CONFIG_TREE_H_
