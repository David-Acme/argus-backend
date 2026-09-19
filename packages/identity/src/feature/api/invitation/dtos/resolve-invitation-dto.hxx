#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct ResolveInvitationDto
{
  std::string token;

  static ResolveInvitationDto fromJson(const Json::Value& json);
};
