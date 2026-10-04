#pragma once

#include <auth/session-platform.hxx>
#include <cstdint>
#include <json/value.h>
#include <string>
#include <vector>

struct SessionView
{
  std::string id;
  SessionPlatform platform{SessionPlatform::Unknown};
  std::string deviceName;
  int64_t createdAt{0};
  int64_t lastSeenAt{0};
  int64_t expiresAt{0};
  bool current{false};
};

struct ResponseListSessionsDto
{
  std::vector<SessionView> sessions;

  [[nodiscard]] Json::Value toJson() const;
};
