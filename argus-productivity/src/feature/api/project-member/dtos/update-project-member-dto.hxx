#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateProjectMemberDto
{
  std::string access;

  static UpdateProjectMemberDto fromJson(const Json::Value& json);
};
