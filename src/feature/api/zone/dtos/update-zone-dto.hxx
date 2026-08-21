#pragma once

#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateZoneDto
{
  std::optional<std::string> name;
  std::optional<std::string> points;
  std::optional<std::string> zoneType;
  std::optional<std::string> color;
  std::optional<bool> isEnabled;

  static UpdateZoneDto fromJson(const Json::Value& json);
};
