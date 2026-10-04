#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>

struct ResponseRtcCallDto
{
  std::string kind;
  std::string summary;
  std::string lang;
  int64_t cameraId{0};
  std::string cameraName;
  int64_t episodeId{0};
};

struct ResponseRtcTokenDto
{
  std::string url;
  std::string token;
  std::string room;
  std::string identity;
  std::string agentIdentity;
  std::string callId;
  int64_t expiresAt{0};
  std::optional<ResponseRtcCallDto> call;

  [[nodiscard]] Json::Value toJson() const;
};
