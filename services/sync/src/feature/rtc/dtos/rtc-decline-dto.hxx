#pragma once

#include <json/value.h>

#include <string>

struct RtcDeclineDto
{
  std::string callId;

  static RtcDeclineDto fromJson(const Json::Value& json);
};
