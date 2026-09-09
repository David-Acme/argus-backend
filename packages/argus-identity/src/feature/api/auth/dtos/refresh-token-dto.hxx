#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct RefreshTokenDto
{
  std::string refreshToken;

  static RefreshTokenDto fromJson(const Json::Value& json);
};
