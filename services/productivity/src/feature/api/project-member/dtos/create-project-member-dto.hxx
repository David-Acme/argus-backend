#pragma once

#include <cstdint>
#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CreateProjectMemberDto
{
  int64_t projectId{0};
  int64_t userId{0};
  std::string access;

  static CreateProjectMemberDto fromJson(const Json::Value& json);
};
