#pragma once

#include <auth/device-login-status.hxx>
#include <auth/user-role.hxx>
#include <cstdint>
#include <json/value.h>
#include <string>

struct DeviceLoginStatusDto
{
  DeviceLoginStatus status{DeviceLoginStatus::Pending};
  std::string accessToken;
  std::string refreshToken;
  int64_t userId{0};
  std::string name;
  std::string deviceSecret;
  UserRole role{UserRole::Guest};

  Json::Value toJson() const;
};
