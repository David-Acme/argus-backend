#pragma once

#include <json/value.h>

#include <optional>
#include <string>

struct RtcTokenDto
{
  std::string callId;
  bool resume{false};
  bool resumeIsBoolean{true};
  std::optional<std::string> mode;

  static RtcTokenDto fromJson(const Json::Value& json);
};
