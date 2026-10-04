#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>
#include <auth/user-role.hxx>

struct CreateInvitationDto
{
  UserRole role{UserRole::Guest};
  std::string roleValue;

  static CreateInvitationDto fromJson(const Json::Value& json);
};
