/// @file docs.h
/// @brief Operator reference documentation, generated from the truth.
///
/// The schema and the command tree already hold every configuration
/// path (type, range, default, help, example) and every verb (args,
/// role gate, help). Hand-written option references rot the moment a
/// feature lands; this generates the reference FROM those structures
/// so it cannot disagree with the binary. Products expose it as
/// `--dump-docs` and check the output into their repo, with a test
/// diffing the two — stale docs fail the build. Narrative guides
/// stay hand-written; wording fixes to the reference belong in the
/// schema help strings and command specs, not in the output file.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_DOCS_H_
#define INCLUDE_EINHEIT_CLI_DOCS_H_

#include <string>

#include "einheit/cli/adapter.h"
#include "einheit/cli/command_tree.h"
#include "einheit/cli/schema.h"

namespace einheit::cli::docs {

/// Render the full operator reference as Markdown: product header,
/// configuration paths (path, type/constraints, default, help,
/// example), then every command grouped into configuration
/// lifecycle / show / operational, with arguments and role gates.
/// Deterministic: all sections sort by path.
/// @param meta Product identity for the header.
/// @param tree The fully-populated command tree (globals + adapter).
/// @param schema The product schema.
/// @returns Markdown document text.
auto GenerateReference(const ProductMetadata &meta,
                       const CommandTree &tree,
                       const schema::Schema &schema) -> std::string;

}  // namespace einheit::cli::docs

#endif  // INCLUDE_EINHEIT_CLI_DOCS_H_
