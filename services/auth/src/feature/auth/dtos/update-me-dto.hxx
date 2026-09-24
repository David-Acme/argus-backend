#pragma once

#include <json/value.h>
#include <optional>
#include <string>
#include <validation/validation_dsl.hxx>

struct UpdateMeDto
{
  std::optional<std::string> name;

  static UpdateMeDto fromJson(const Json::Value& json);
};
