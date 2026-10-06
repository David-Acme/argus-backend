#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <json/value.h>
#include <string>

struct ResponseLoginDto
{
  std::string accessToken;
  std::string refreshToken;
  int64_t userId{0};
  std::string name;
  UserRole role{UserRole::Unknown};
  int64_t personId{0};
  bool alreadyRegistered{false};
  std::string deviceSecret;

  Json::Value toJson() const;
};
