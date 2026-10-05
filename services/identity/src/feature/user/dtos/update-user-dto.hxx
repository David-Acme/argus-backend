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
  std::optional<std::string> role;
  std::optional<UserRole> userRole;
  std::optional<bool> isActive;

  static UpdateUserDto fromJson(const Json::Value& json);
};
