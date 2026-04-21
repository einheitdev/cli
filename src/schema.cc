/// @file schema.cc
/// @brief Schema loader, validator, and completion walker.
// Copyright (c) 2026 Einheit Networks

#include "einheit/cli/schema.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "einheit/cli/fuzzy.h"
#include "einheit/cli/net_parse.h"

namespace einheit::cli::schema {
namespace {

auto MakeError(SchemaError code, std::string message)
    -> Error<SchemaError> {
  return Error<SchemaError>{code, std::move(message)};
}

auto PrimitiveFromString(const std::string &s)
    -> std::optional<PrimitiveType> {
  if (s == "string") return PrimitiveType::String;
  if (s == "integer") return PrimitiveType::Integer;
  if (s == "boolean" || s == "bool") return PrimitiveType::Boolean;
  if (s == "cidr") return PrimitiveType::Cidr;
  if (s == "ip" || s == "ip_address") return PrimitiveType::IpAddress;
  if (s == "enum") return PrimitiveType::EnumStr;
  return std::nullopt;
}

auto ParseNode(const YAML::Node &y)
    -> std::expected<std::unique_ptr<Node>, Error<SchemaError>>;

auto FillCommon(Node &out, const YAML::Node &y) -> void {
  if (y["help"]) out.help = y["help"].as<std::string>();
  if (y["example"]) out.example = y["example"].as<std::string>();
  if (y["required"]) out.required = y["required"].as<bool>();
  if (y["default"]) {
    out.default_value = y["default"].as<std::string>();
  }
}

auto ParsePrimitive(const YAML::Node &y, PrimitiveType p)
    -> std::expected<PrimitiveSpec, Error<SchemaError>> {
  PrimitiveSpec ps;
  ps.type = p;
  if (p == PrimitiveType::EnumStr && y["values"]) {
    for (const auto &v : y["values"]) {
      ps.values.push_back(v.as<std::string>());
    }
  }
  if (y["range"] && y["range"].IsSequence() && y["range"].size() == 2) {
    ps.range = std::pair<std::int64_t, std::int64_t>{
        y["range"][0].as<std::int64_t>(),
        y["range"][1].as<std::int64_t>()};
  }
  if (y["pattern"]) ps.pattern = y["pattern"].as<std::string>();
  return ps;
}

auto ParseObject(const YAML::Node &y)
    -> std::expected<ObjectSpec, Error<SchemaError>> {
  ObjectSpec obj;
  const YAML::Node &fields = y["fields"] ? y["fields"] : y;
  if (!fields.IsMap()) {
    return std::unexpected(MakeError(
        SchemaError::ValidationFailed, "object needs map"));
  }
  for (const auto &kv : fields) {
    const auto key = kv.first.as<std::string>();
    auto child = ParseNode(kv.second);
    if (!child) return std::unexpected(child.error());
    obj.fields.emplace(key, std::move(*child));
  }
  return obj;
}

auto ParseNode(const YAML::Node &y)
    -> std::expected<std::unique_ptr<Node>, Error<SchemaError>> {
  auto node = std::make_unique<Node>();
  FillCommon(*node, y);

  if (!y["type"]) {
    // Bare map of subfields — treat as an implicit object.
    auto obj = ParseObject(y);
    if (!obj) return std::unexpected(obj.error());
    node->shape = std::move(*obj);
    return node;
  }

  const auto type_str = y["type"].as<std::string>();
  if (auto p = PrimitiveFromString(type_str); p) {
    auto prim = ParsePrimitive(y, *p);
    if (!prim) return std::unexpected(prim.error());
    node->shape = std::move(*prim);
    return node;
  }

  if (type_str == "list") {
    ListSpec ls;
    if (y["item"]) {
      auto child = ParseNode(y["item"]);
      if (!child) return std::unexpected(child.error());
      ls.item = std::move(*child);
    }
    node->shape = std::move(ls);
    return node;
  }

  if (type_str == "map") {
    MapSpec ms;
    if (y["key"]) {
      if (auto p = PrimitiveFromString(y["key"].as<std::string>()); p) {
        ms.key_type = *p;
      }
    }
    if (y["value"]) {
      auto child = ParseNode(y["value"]);
      if (!child) return std::unexpected(child.error());
      ms.value = std::move(*child);
    }
    node->shape = std::move(ms);
    return node;
  }

  if (type_str == "object") {
    auto obj = ParseObject(y);
    if (!obj) return std::unexpected(obj.error());
    node->shape = std::move(*obj);
    return node;
  }

  // Anything else is a custom adapter-declared type name.
  node->shape = CustomTypeSpec{type_str};
  return node;
}

// Walk a dotted path through the schema tree. Tokens are consumed
// one at a time against the current node shape: Object fields are
// looked up by name, Map keys consume any token, List indices have
// already been stripped during tokenisation.
auto Resolve(const Schema &schema, const std::string &path)
    -> const Node * {
  std::vector<std::string> parts;
  {
    std::string token;
    for (const char c : path) {
      if (c == '.' || c == '[') {
        if (!token.empty()) { parts.push_back(token); token.clear(); }
      } else if (c == ']') {
        // index segment boundary; discard the (numeric) contents
      } else {
        token.push_back(c);
      }
    }
    if (!token.empty()) parts.push_back(token);
  }

  const Node *cur_node = nullptr;
  const ObjectSpec *cur_obj = &schema.root;
  for (const auto &p : parts) {
    if (cur_obj) {
      auto it = cur_obj->fields.find(p);
      if (it == cur_obj->fields.end()) return nullptr;
      cur_node = it->second.get();
    } else if (cur_node) {
      // Opaque map key or list index: step into container value.
      if (auto *m = std::get_if<MapSpec>(&cur_node->shape)) {
        cur_node = m->value.get();
      } else if (auto *lst = std::get_if<ListSpec>(&cur_node->shape)) {
        cur_node = lst->item.get();
      } else {
        return nullptr;
      }
      if (!cur_node) return nullptr;
    } else {
      return nullptr;
    }

    cur_obj = cur_node
                  ? std::get_if<ObjectSpec>(&cur_node->shape)
                  : nullptr;
  }
  return cur_node;
}

auto ValidateLeaf(const PrimitiveSpec &ps, const std::string &value)
    -> std::expected<void, Error<SchemaError>> {
  switch (ps.type) {
    case PrimitiveType::String:
      return {};
    case PrimitiveType::Boolean: {
      if (value == "true" || value == "false") return {};
      return std::unexpected(
          MakeError(SchemaError::ValidationFailed, "expected bool"));
    }
    case PrimitiveType::Integer: {
      try {
        const auto n = std::stoll(value);
        if (ps.range && (n < ps.range->first || n > ps.range->second)) {
          return std::unexpected(MakeError(
              SchemaError::ValidationFailed, "out of range"));
        }
        return {};
      } catch (...) {
        return std::unexpected(MakeError(
            SchemaError::ValidationFailed, "expected integer"));
      }
    }
    case PrimitiveType::EnumStr: {
      if (std::find(ps.values.begin(), ps.values.end(), value) ==
          ps.values.end()) {
        return std::unexpected(MakeError(
            SchemaError::ValidationFailed, "value not in enum"));
      }
      return {};
    }
    case PrimitiveType::IpAddress:
      if (!net_parse::IsIpAddress(value)) {
        return std::unexpected(MakeError(
            SchemaError::ValidationFailed, "malformed ip address"));
      }
      return {};
    case PrimitiveType::Cidr:
      if (!net_parse::IsCidr(value)) {
        return std::unexpected(MakeError(
            SchemaError::ValidationFailed, "malformed cidr"));
      }
      return {};
  }
  return {};
}

}  // namespace

auto LoadSchema(const std::string &yaml_path)
    -> std::expected<std::shared_ptr<Schema>, Error<SchemaError>> {
  try {
    auto doc = YAML::LoadFile(yaml_path);
    if (!doc["version"] || !doc["product"]) {
      return std::unexpected(MakeError(
          SchemaError::MissingField, "version/product required"));
    }
    auto out = std::make_shared<Schema>();
    out->version = doc["version"].as<std::uint32_t>();
    out->product = doc["product"].as<std::string>();
    if (doc["config"]) {
      auto obj = ParseObject(doc["config"]);
      if (!obj) return std::unexpected(obj.error());
      out->root = std::move(*obj);
    }
    if (doc["types"] && doc["types"].IsMap()) {
      for (const auto &kv : doc["types"]) {
        auto child = ParseNode(kv.second);
        if (!child) return std::unexpected(child.error());
        out->types.emplace(kv.first.as<std::string>(),
                           std::move(*child));
      }
    }
    return out;
  } catch (const YAML::Exception &e) {
    return std::unexpected(
        MakeError(SchemaError::YamlParseFailed, e.what()));
  } catch (const std::exception &e) {
    return std::unexpected(
        MakeError(SchemaError::ValidationFailed, e.what()));
  }
}

auto ValidatePath(const Schema &schema, const std::string &path,
                  const std::string &value)
    -> std::expected<void, Error<SchemaError>> {
  const Node *n = Resolve(schema, path);
  if (!n) {
    // Offer a suggestion if the last segment looks like a typo of a
    // known field at the parent scope.
    std::string message = std::format("unknown path: {}", path);
    const auto dot = path.find_last_of('.');
    const std::string parent =
        (dot == std::string::npos) ? "" : path.substr(0, dot);
    const std::string leaf =
        (dot == std::string::npos) ? path : path.substr(dot + 1);
    auto candidates = Completions(
        schema, parent.empty() ? "" : std::format("{}.", parent));
    auto hints = fuzzy::Suggest(leaf, candidates);
    if (!hints.empty()) {
      const std::string full =
          parent.empty()
              ? hints.front()
              : std::format("{}.{}", parent, hints.front());
      message = std::format("{} — did you mean '{}'?", message, full);
    }
    return std::unexpected(
        MakeError(SchemaError::ValidationFailed, std::move(message)));
  }
  if (auto *ps = std::get_if<PrimitiveSpec>(&n->shape)) {
    return ValidateLeaf(*ps, value);
  }
  if (std::get_if<CustomTypeSpec>(&n->shape)) {
    // Custom types are adapter-defined; accept without client check.
    // The daemon performs authoritative validation.
    return {};
  }
  return std::unexpected(MakeError(
      SchemaError::ValidationFailed, "path is not a leaf"));
}

namespace {

auto TypeDescriptor(const PrimitiveSpec &ps) -> std::string {
  switch (ps.type) {
    case PrimitiveType::String:    return "string";
    case PrimitiveType::Integer: {
      if (ps.range) {
        return std::format("integer [{}..{}]",
                           ps.range->first, ps.range->second);
      }
      return "integer";
    }
    case PrimitiveType::Boolean:   return "boolean";
    case PrimitiveType::Cidr:      return "cidr";
    case PrimitiveType::IpAddress: return "ip";
    case PrimitiveType::EnumStr: {
      std::string s = "enum {";
      for (std::size_t i = 0; i < ps.values.size(); ++i) {
        if (i > 0) s += ", ";
        s += ps.values[i];
      }
      s += "}";
      return s;
    }
  }
  return "?";
}

struct Entry {
  std::string path;
  std::string type_desc;
  std::string default_value;
  std::string help;
};

auto WalkNode(const Node *node, const std::string &prefix,
              std::vector<Entry> &out) -> void;

auto WalkObject(const ObjectSpec &obj, const std::string &prefix,
                std::vector<Entry> &out) -> void {
  // Deterministic order — sort keys for reproducible output.
  std::vector<std::string> keys;
  keys.reserve(obj.fields.size());
  for (const auto &[k, _] : obj.fields) keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  for (const auto &k : keys) {
    const auto &child = obj.fields.at(k);
    const auto next = prefix.empty()
                          ? k
                          : std::format("{}.{}", prefix, k);
    WalkNode(child.get(), next, out);
  }
}

auto WalkNode(const Node *node, const std::string &prefix,
              std::vector<Entry> &out) -> void {
  if (!node) return;

  Entry e;
  e.path = prefix;
  e.help = node->help;
  e.default_value = node->default_value.value_or("");

  if (auto *ps = std::get_if<PrimitiveSpec>(&node->shape)) {
    e.type_desc = TypeDescriptor(*ps);
    out.push_back(std::move(e));
    return;
  }
  if (auto *ct = std::get_if<CustomTypeSpec>(&node->shape)) {
    e.type_desc = std::format("custom<{}>", ct->name);
    out.push_back(std::move(e));
    return;
  }
  if (auto *obj = std::get_if<ObjectSpec>(&node->shape)) {
    e.type_desc = "object";
    // Emit the container itself only when it carries help worth
    // surfacing — otherwise skip and just show its children.
    if (!e.help.empty() && !prefix.empty()) out.push_back(e);
    WalkObject(*obj, prefix, out);
    return;
  }
  if (auto *lst = std::get_if<ListSpec>(&node->shape)) {
    e.type_desc = "list";
    if (!e.help.empty() && !prefix.empty()) out.push_back(e);
    if (lst->item) {
      WalkNode(lst->item.get(),
               std::format("{}[0]", prefix), out);
    }
    return;
  }
  if (auto *m = std::get_if<MapSpec>(&node->shape)) {
    e.type_desc = "map";
    if (!e.help.empty() && !prefix.empty()) out.push_back(e);
    if (m->value) {
      WalkNode(m->value.get(),
               std::format("{}.<name>", prefix), out);
    }
    return;
  }
}

}  // namespace

auto FormatSchema(const Schema &schema, const std::string &prefix)
    -> std::string {
  std::vector<Entry> entries;
  WalkObject(schema.root, "", entries);

  // Filter by prefix if requested.
  if (!prefix.empty()) {
    std::erase_if(entries, [&](const Entry &e) {
      return e.path.rfind(prefix, 0) != 0;
    });
  }

  if (entries.empty()) {
    return prefix.empty()
               ? "schema is empty\n"
               : std::format("no paths under '{}'\n", prefix);
  }

  // Column widths.
  std::size_t w_path = 4;   // "path"
  std::size_t w_type = 4;   // "type"
  std::size_t w_default = 7;  // "default"
  for (const auto &e : entries) {
    w_path = std::max(w_path, e.path.size());
    w_type = std::max(w_type, e.type_desc.size());
    w_default = std::max(w_default, e.default_value.size());
  }

  std::string out;
  out += std::format("{:<{}}  {:<{}}  {:<{}}  {}\n", "path", w_path,
                     "type", w_type, "default", w_default, "help");
  out += std::format("{}  {}  {}  {}\n", std::string(w_path, '-'),
                     std::string(w_type, '-'),
                     std::string(w_default, '-'),
                     std::string(4, '-'));
  for (const auto &e : entries) {
    out += std::format("{:<{}}  {:<{}}  {:<{}}  {}\n", e.path,
                       w_path, e.type_desc, w_type,
                       e.default_value, w_default, e.help);
  }
  return out;
}

auto Completions(const Schema &schema,
                 const std::string &partial_path)
    -> std::vector<std::string> {
  // Split the partial into "resolved prefix" + "typing suffix".
  const auto dot = partial_path.find_last_of('.');
  const std::string prefix =
      (dot == std::string::npos) ? "" : partial_path.substr(0, dot);
  const std::string typing =
      (dot == std::string::npos) ? partial_path
                                  : partial_path.substr(dot + 1);

  const ObjectSpec *scope = &schema.root;
  if (!prefix.empty()) {
    const Node *n = Resolve(schema, prefix);
    if (!n) return {};
    if (auto *obj = std::get_if<ObjectSpec>(&n->shape)) {
      scope = obj;
    } else if (auto *lst = std::get_if<ListSpec>(&n->shape)) {
      if (!lst->item) return {};
      scope = std::get_if<ObjectSpec>(&lst->item->shape);
    } else if (auto *m = std::get_if<MapSpec>(&n->shape)) {
      if (!m->value) return {};
      scope = std::get_if<ObjectSpec>(&m->value->shape);
    } else {
      return {};
    }
  }
  if (!scope) return {};

  std::vector<std::string> out;
  for (const auto &[name, child] : scope->fields) {
    if (name.rfind(typing, 0) == 0) out.push_back(name);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace einheit::cli::schema
