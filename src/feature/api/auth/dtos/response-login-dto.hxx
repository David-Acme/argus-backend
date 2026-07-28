#pragma once

#include <cstdint>
#include <json/value.h>
#include <shared/enums.hxx>
#include <string>

struct ResponseLoginDto
{
  std::string accessToken;
  std::string refreshToken;
  int64_t userId;
  std::string name;
  UserRole role;
  int64_t personId;

  Json::Value toJson() const;
};
