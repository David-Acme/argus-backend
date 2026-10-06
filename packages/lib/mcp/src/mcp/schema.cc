#include "schema.hxx"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace argus::mcp::schema
{

namespace
{
constexpr int kMaxDepth = 16;
constexpr std::string_view kDraft07 = "http://json-schema.org/draft-07/schema#";
constexpr std::string_view kDraft202012 = "https://json-schema.org/draft/2020-12/schema";

struct Cursor
{
  const Json::Value& schema;
  const Json::Value& value;
  std::string path;
  int depth{0};
};

std::string quoted(const std::string& path)
{
  return "argument '" + path + "'";
}

std::string article(std::string_view type)
{
  if (type == "integer" || type == "object" || type == "array")
    return "an " + std::string(type);
  if (type == "null")
    return "null";
  return "a " + std::string(type);
}

std::string childPath(const std::string& path, const std::string& name)
{
  return path.empty() ? name : path + "." + name;
}

bool matchesType(const Json::Value& value, std::string_view type)
{
  if (type == "object")
    return value.isObject();
  if (type == "array")
    return value.isArray();
  if (type == "string")
    return value.isString();
  if (type == "boolean")
    return value.isBool();
  if (type == "null")
    return value.isNull();
  if (type == "number")
    return value.isNumeric() && !value.isBool();
  if (type == "integer")
    return value.isIntegral() || (value.isDouble() && std::floor(value.asDouble()) == value.asDouble());
  return false;
}

std::vector<std::string> typesOf(const Json::Value& schema)
{
  std::vector<std::string> types;
  if (schema["type"].isString())
    types.push_back(schema["type"].asString());
  else if (schema["type"].isArray())
    for (const auto& entry : schema["type"])
      if (entry.isString())
        types.push_back(entry.asString());
  return types;
}

size_t codePoints(const std::string& text)
{
  return static_cast<size_t>(std::ranges::count_if(
      text, [](char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U; }));
}

std::string subject(const std::string& path)
{
  return path.empty() ? "arguments" : quoted(path);
}

std::optional<std::string> check(const Cursor& at);

std::optional<std::string> typeViolation(const Cursor& at)
{
  const auto types = typesOf(at.schema);
  if (types.empty() ||
      std::ranges::any_of(types, [&at](const std::string& type) { return matchesType(at.value, type); }))
    return std::nullopt;
  if (at.path.empty() && types.front() == "object")
    return "arguments must be a JSON object";
  return subject(at.path) + " must be " + article(types.front());
}

std::optional<std::string> enumViolation(const Cursor& at)
{
  if (at.schema.isMember("const") && at.schema["const"] != at.value)
    return subject(at.path) + " has an invalid value";
  if (!at.schema["enum"].isArray())
    return std::nullopt;
  const bool known = std::ranges::any_of(at.schema["enum"], [&at](const Json::Value& allowed) {
    return allowed == at.value;
  });
  return known ? std::nullopt : std::optional<std::string>(subject(at.path) + " has an invalid value");
}

std::optional<std::string> stringViolation(const Cursor& at)
{
  if (!at.value.isString())
    return std::nullopt;
  const size_t length = codePoints(at.value.asString());
  if (at.schema["minLength"].isIntegral() && length < at.schema["minLength"].asUInt64())
    return subject(at.path) + " is too short";
  if (at.schema["maxLength"].isIntegral() && length > at.schema["maxLength"].asUInt64())
    return subject(at.path) + " is too long";
  return std::nullopt;
}

std::optional<std::string> numberViolation(const Cursor& at)
{
  if (!at.value.isNumeric() || at.value.isBool())
    return std::nullopt;
  const double number = at.value.asDouble();
  if (at.schema["minimum"].isNumeric() && number < at.schema["minimum"].asDouble())
    return subject(at.path) + " is below the minimum";
  if (at.schema["maximum"].isNumeric() && number > at.schema["maximum"].asDouble())
    return subject(at.path) + " is above the maximum";
  if (at.schema["exclusiveMinimum"].isNumeric() && number <= at.schema["exclusiveMinimum"].asDouble())
    return subject(at.path) + " is below the minimum";
  if (at.schema["exclusiveMaximum"].isNumeric() && number >= at.schema["exclusiveMaximum"].asDouble())
    return subject(at.path) + " is above the maximum";
  return std::nullopt;
}

std::optional<std::string> arrayViolation(const Cursor& at)
{
  if (!at.value.isArray())
    return std::nullopt;
  const Json::ArrayIndex size = at.value.size();
  if (at.schema["minItems"].isIntegral() && size < at.schema["minItems"].asUInt64())
    return subject(at.path) + " has too few items";
  if (at.schema["maxItems"].isIntegral() && size > at.schema["maxItems"].asUInt64())
    return subject(at.path) + " has too many items";
  if (!at.schema["items"].isObject())
    return std::nullopt;
  for (Json::ArrayIndex index = 0; index < size; ++index) {
    if (auto found = check({.schema = at.schema["items"],
                            .value = at.value[index],
                            .path = at.path + "[" + std::to_string(index) + "]",
                            .depth = at.depth + 1}))
      return found;
  }
  return std::nullopt;
}

std::optional<std::string> objectViolation(const Cursor& at)
{
  if (!at.value.isObject())
    return std::nullopt;
  const Json::Value& properties = at.schema["properties"];
  for (const auto& required : at.schema["required"])
    if (required.isString() && !at.value.isMember(required.asString()))
      return "missing required argument '" + childPath(at.path, required.asString()) + "'";
  for (const auto& name : at.value.getMemberNames()) {
    if (properties.isObject() && properties.isMember(name)) {
      if (auto found = check({.schema = properties[name],
                              .value = at.value[name],
                              .path = childPath(at.path, name),
                              .depth = at.depth + 1}))
        return found;
      continue;
    }
    const Json::Value& extra = at.schema["additionalProperties"];
    if (extra.isBool() && !extra.asBool())
      return quoted(childPath(at.path, name)) + " is not accepted";
    if (extra.isObject()) {
      if (auto found = check({.schema = extra,
                              .value = at.value[name],
                              .path = childPath(at.path, name),
                              .depth = at.depth + 1}))
        return found;
    }
  }
  return std::nullopt;
}

std::optional<std::string> compositionViolation(const Cursor& at)
{
  for (const auto& branch : at.schema["allOf"])
    if (auto found = check({.schema = branch, .value = at.value, .path = at.path, .depth = at.depth + 1}))
      return found;
  const auto matching = [&at](const Json::Value& branches) {
    return std::ranges::count_if(branches, [&at](const Json::Value& branch) {
      return !check({.schema = branch, .value = at.value, .path = at.path, .depth = at.depth + 1}).has_value();
    });
  };
  if (at.schema["anyOf"].isArray() && matching(at.schema["anyOf"]) == 0)
    return subject(at.path) + " has an invalid value";
  if (at.schema["oneOf"].isArray() && matching(at.schema["oneOf"]) != 1)
    return subject(at.path) + " has an invalid value";
  return std::nullopt;
}

std::optional<std::string> check(const Cursor& at)
{
  if (at.depth > kMaxDepth)
    return subject(at.path) + " is nested too deeply";
  if (at.schema.isBool())
    return at.schema.asBool() ? std::nullopt : std::optional<std::string>(subject(at.path) + " is not accepted");
  if (!at.schema.isObject())
    return std::nullopt;
  constexpr std::array<std::optional<std::string> (*)(const Cursor&), 7> kChecks{
      typeViolation,  enumViolation,   stringViolation, numberViolation,
      arrayViolation, objectViolation, compositionViolation};
  for (const auto verify : kChecks)
    if (auto found = verify(at))
      return found;
  return std::nullopt;
}

struct Walk
{
  const Json::Value& schema;
  int depth{0};
};

std::optional<std::string> unsupportedAt(const Walk& walk);

std::optional<std::string> unsupportedChildren(const Walk& walk)
{
  const Json::Value& schema = walk.schema;
  for (const char* keyword : {"items", "additionalProperties"})
    if (schema[keyword].isObject())
      if (auto found = unsupportedAt({.schema = schema[keyword], .depth = walk.depth + 1}))
        return found;
  if (schema["properties"].isObject())
    for (const auto& name : schema["properties"].getMemberNames())
      if (auto found = unsupportedAt({.schema = schema["properties"][name], .depth = walk.depth + 1}))
        return found;
  for (const char* keyword : {"allOf", "anyOf", "oneOf"})
    for (const auto& branch : schema[keyword])
      if (auto found = unsupportedAt({.schema = branch, .depth = walk.depth + 1}))
        return found;
  return std::nullopt;
}

std::optional<std::string> unsupportedAt(const Walk& walk)
{
  if (walk.depth > kMaxDepth)
    return "the schema is nested too deeply";
  if (!walk.schema.isObject())
    return std::nullopt;
  if (walk.schema.isMember("$ref") || walk.schema.isMember("$dynamicRef"))
    return "the schema holds a reference, which is not resolved";
  if (walk.schema.isMember("$schema") && walk.schema["$schema"].asString() != kDraft07 &&
      walk.schema["$schema"].asString() != kDraft202012)
    return "the schema dialect is not supported";
  return unsupportedChildren(walk);
}

Json::Value typed(std::string_view type, const Facets& facets)
{
  Json::Value out(Json::objectValue);
  out["type"] = std::string(type);
  if (!facets.description.empty())
    out["description"] = facets.description;
  return out;
}
}

Json::Value object(const std::vector<Property>& properties)
{
  Json::Value out(Json::objectValue);
  out["type"] = "object";
  Json::Value members(Json::objectValue);
  Json::Value required(Json::arrayValue);
  for (const auto& property : properties) {
    members[property.name] = property.schema;
    if (property.required)
      required.append(property.name);
  }
  out["properties"] = std::move(members);
  if (!required.empty())
    out["required"] = std::move(required);
  return out;
}

Json::Value emptyObject()
{
  Json::Value out(Json::objectValue);
  out["type"] = "object";
  out["additionalProperties"] = false;
  return out;
}

Json::Value text(const Facets& facets)
{
  Json::Value out = typed("string", facets);
  if (facets.minimum)
    out["minLength"] = *facets.minimum;
  if (facets.maximum)
    out["maxLength"] = *facets.maximum;
  return out;
}

Json::Value integer(const Facets& facets)
{
  Json::Value out = typed("integer", facets);
  if (facets.minimum)
    out["minimum"] = *facets.minimum;
  if (facets.maximum)
    out["maximum"] = *facets.maximum;
  return out;
}

Json::Value number(const Facets& facets)
{
  Json::Value out = typed("number", facets);
  if (facets.minimum)
    out["minimum"] = *facets.minimum;
  if (facets.maximum)
    out["maximum"] = *facets.maximum;
  return out;
}

Json::Value boolean(const Facets& facets)
{
  return typed("boolean", facets);
}

Json::Value choice(const std::vector<std::string>& values, const Facets& facets)
{
  Json::Value out = typed("string", facets);
  Json::Value allowed(Json::arrayValue);
  for (const auto& value : values)
    allowed.append(value);
  out["enum"] = std::move(allowed);
  return out;
}

Json::Value list(const Json::Value& items, const Facets& facets)
{
  Json::Value out = typed("array", facets);
  out["items"] = items;
  if (facets.minimum)
    out["minItems"] = *facets.minimum;
  if (facets.maximum)
    out["maxItems"] = *facets.maximum;
  return out;
}

std::optional<std::string> unsupported(const Json::Value& schema)
{
  if (!schema.isObject() || !schema["type"].isString() || schema["type"].asString() != "object")
    return "the input schema must be an object schema";
  return unsupportedAt({.schema = schema, .depth = 0});
}

std::optional<std::string> violation(const Json::Value& schema, const Json::Value& value)
{
  return check({.schema = schema, .value = value, .path = {}, .depth = 0});
}

}
