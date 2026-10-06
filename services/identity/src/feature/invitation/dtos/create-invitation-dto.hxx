#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>
#include <auth/user-role.hxx>

struct CreateInvitationDto
{
  std::string role;
  UserRole userRole{UserRole::Unknown};

  static CreateInvitationDto fromJson(const Json::Value& json);
};
