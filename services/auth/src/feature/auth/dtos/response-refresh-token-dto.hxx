#pragma once

#include <json/value.h>
#include <string>

struct ResponseRefreshTokenDto
{
  std::string accessToken;
  std::string refreshToken;

  Json::Value toJson() const;
};
