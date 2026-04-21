/// @file schema.h
/// @brief Config schema — the single source of truth for validation,
/// tab completion, and help text.
///
/// Data-oriented: the schema is a tree of plain structs. Free
/// functions walk it to validate, complete, and render. No virtuals,
/// no inheritance; the loader reads a yaml-cpp document and emits
/// this tree.
// Copyright (c) 2026 Einheit Networks

#ifndef INCLUDE_EINHEIT_CLI_SCHEMA_H_
#define INCLUDE_EINHEIT_CLI_SCHEMA_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "einheit/cli/error.h"

namespace einheit::cli::schema {

/// Errors raised by schema loading or validation.
enum class SchemaError {
  /// YAML could not be parsed.
  YamlParseFailed,
  /// Required top-level field missing (version, product).
  MissingField,
  /// A $ref target was not resolvable.
  UnresolvedRef,
  /// A value did not satisfy its declared type/constraint.
  ValidationFailed,
  /// Custom (adapter-provided) type validator rejected a value.
  CustomTypeRejected,
  /// Unknown type name in the schema.
  UnknownType,
};

/// Built-in primitive types.
enum class PrimitiveType {
  String,
  Integer,
  Boolean,
  Cidr,
  IpAddress,
  EnumStr,
};

/// Forward-declared recursive node type.
struct Node;

/// A list-type value: each element conforms to `item`.
struct ListSpec {
  std::unique_ptr<Node> item;
};

/// A map-type value: keys are validated against `key_type`, values
/// against `value`.
struct MapSpec {
  PrimitiveType key_type = PrimitiveType::String;
  std::unique_ptr<Node> value;
};

/// An object-type value: a fixed set of named fields.
struct ObjectSpec {
  std::unordered_map<std::string, std::unique_ptr<Node>> fields;
};

/// A primitive leaf value. `values` populated only for EnumStr.
struct PrimitiveSpec {
  PrimitiveType type = PrimitiveType::String;
  std::vector<std::string> values;
  std::optional<std::pair<std::int64_t, std::int64_t>> range;
  std::optional<std::string> pattern;
};

/// A named custom type from the adapter (e.g. "fleet_address").
/// The runtime resolves this against an adapter-supplied validator.
struct CustomTypeSpec {
  std::string name;
};

/// One node in the schema tree.
struct Node {
  /// Field help shown by `?`.
  std::string help;
  /// Example value rendered in help output.
  std::string example;
  /// Whether the field must be present after a commit.
  bool required = false;
  /// Optional stringified default value.
  std::optional<std::string> default_value;
  /// The actual shape of the node.
  std::variant<PrimitiveSpec, ListSpec, MapSpec, ObjectSpec,
               CustomTypeSpec>
      shape;
};

/// Top-level parsed schema.
struct Schema {
  /// Schema format version.
  std::uint32_t version = 1;
  /// Product identifier (e.g. "g-gateway").
  std::string product;
  /// Root object — corresponds to the YAML `config:` tree.
  ObjectSpec root;
  /// Named reusable type fragments under `types:`.
  std::unordered_map<std::string, std::unique_ptr<Node>> types;
};

/// Parse a YAML schema document from disk.
/// @param yaml_path Path to the schema YAML file.
/// @returns Parsed Schema or SchemaError.
auto LoadSchema(const std::string &yaml_path)
    -> std::expected<std::shared_ptr<Schema>, Error<SchemaError>>;

/// Validate a user-entered value at the given dotted path against
/// the schema. Used by the `set` client-side validator.
/// @param schema Schema to validate against.
/// @param path Dotted path (e.g. "routes[0].via_peer").
/// @param value Raw string value from the command line.
/// @returns void on success or SchemaError with context.
auto ValidatePath(const Schema &schema, const std::string &path,
                  const std::string &value)
    -> std::expected<void, Error<SchemaError>>;

/// Compute tab-completion candidates for a partial dotted path.
/// @param schema Schema to walk.
/// @param partial_path Whatever the user has typed so far.
/// @returns Candidate completions, possibly empty.
auto Completions(const Schema &schema,
                 const std::string &partial_path)
    -> std::vector<std::string>;

/// Walk every leaf and container in the schema and return a plain-
/// text description: path, type (with range / enum values / format),
/// default, and help text. Kept for tests + doc generation.
/// @param schema Schema to describe.
/// @param prefix When non-empty, only paths under `prefix` are shown.
/// @returns Multi-line string ending with a trailing newline.
auto FormatSchema(const Schema &schema,
                  const std::string &prefix = "") -> std::string;

}  // namespace einheit::cli::schema

#include "einheit/cli/render/table.h"

namespace einheit::cli::schema {

/// Build a rendering Table describing the schema. Intended for
/// `show schema` so the shell can pipe it through RenderFormatted
/// and pick up colours + format switching.
/// @param schema Schema to describe.
/// @param prefix When non-empty, only paths under `prefix` are shown.
/// @returns Table with columns `path`, `type`, `default`, `help`.
auto BuildSchema(const Schema &schema,
                 const std::string &prefix = "") -> render::Table;

}  // namespace einheit::cli::schema

#endif  // INCLUDE_EINHEIT_CLI_SCHEMA_H_
