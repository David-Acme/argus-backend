#pragma once

#include "update-environment-dto.hxx"

#include <json/value.h>
#include <optional>
#include <string>

struct CreateEnvironmentDto
{
  UpdateEnvironmentDto fields;
  std::optional<std::string> mode;

  static CreateEnvironmentDto fromJson(const Json::Value& json);
};
