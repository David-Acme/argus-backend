#pragma once

#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateMeDto
{
  std::optional<std::string> name;

  static UpdateMeDto fromJson(const Json::Value& json);
};