#pragma once

#include <auth/session-origin.hxx>
#include <auth/session-platform.hxx>
#include <cstdint>
#include <json/value.h>
#include <string>

struct ResponseDeviceLoginDetailsDto
{
  std::string challengeId;
  SessionPlatform platform{SessionPlatform::Unknown};
  std::string deviceName;
  SessionOrigin origin{SessionOrigin::Unknown};
  std::string ipAddress;
  int64_t createdAt{0};
  int64_t expiresAt{0};

  Json::Value toJson() const;
};
