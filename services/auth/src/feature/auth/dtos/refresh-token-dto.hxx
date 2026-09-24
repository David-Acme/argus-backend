#pragma once

#include <json/value.h>
#include <string>
#include <validation/validation_dsl.hxx>

struct RefreshTokenDto
{
  std::string refreshToken;

  static RefreshTokenDto fromJson(const Json::Value& json);
};
