#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace argus::mcp::schema
{

struct Property
{
  std::string name;
  Json::Value schema;
  bool required{false};
};

struct Facets
{
  std::string description;
  std::optional<int64_t> minimum;
  std::optional<int64_t> maximum;
};

[[nodiscard]] Json::Value object(const std::vector<Property>& properties);

[[nodiscard]] Json::Value emptyObject();

[[nodiscard]] Json::Value text(const Facets& facets = {});

[[nodiscard]] Json::Value integer(const Facets& facets = {});

[[nodiscard]] Json::Value number(const Facets& facets = {});

[[nodiscard]] Json::Value boolean(const Facets& facets = {});

[[nodiscard]] Json::Value choice(const std::vector<std::string>& values, const Facets& facets = {});

[[nodiscard]] Json::Value list(const Json::Value& items, const Facets& facets = {});

[[nodiscard]] std::optional<std::string> unsupported(const Json::Value& schema);

[[nodiscard]] std::optional<std::string> violation(const Json::Value& schema, const Json::Value& value);

}
