#pragma once

#include <json/value.h>
#include <optional>
#include <validation/validation_dsl.hxx>
#include <string>
#include <auth/user-role.hxx>

struct UpdateUserDto
{
  std::optional<std::string> name;
  std::optional<std::string> lastName;
  std::optional<UserRole> role;
  std::optional<std::string> roleValue;
  std::optional<bool> isActive;

  static UpdateUserDto fromJson(const Json::Value& json);
};
