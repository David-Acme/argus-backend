#pragma once

#include <cstdint>
#include <json/value.h>
#include <shared/enums.hxx>
#include <string>

struct DeviceLoginStatusDto
{
  std::string status; // pending | approved | expired
  std::string accessToken;
  std::string refreshToken;
  int64_t userId{0};
  std::string name;
  std::string deviceSecret;
  UserRole role{UserRole::Guest};

  Json::Value toJson() const;
};
