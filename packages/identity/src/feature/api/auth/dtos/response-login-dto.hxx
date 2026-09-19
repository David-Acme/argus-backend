#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>
#include <user-role.hxx>

struct ResponseLoginDto
{
  std::string accessToken;
  std::string refreshToken;
  int64_t userId;
  std::string name;
  UserRole role;
  int64_t personId;
  bool alreadyRegistered = false;
  std::string deviceSecret;

  Json::Value toJson() const;
};
