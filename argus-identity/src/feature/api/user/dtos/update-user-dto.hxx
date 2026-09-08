#pragma once

#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateUserDto
{
  std::optional<std::string> name;
  std::optional<std::string> lastName;
  std::optional<UserRole> role;
  std::optional<std::string> roleValue;
  std::optional<bool> isActive;

  static UpdateUserDto fromJson(const Json::Value& json);
};
