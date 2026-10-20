#pragma once

#include <json/value.h>

#include <cstdint>
#include <string>

struct SendMessageTarget
{
  int64_t counterpartId{0};
  int64_t maxChars{0};
};

struct SendMessageDto
{
  int64_t counterpartId{0};
  std::string body;

  static SendMessageDto fromJson(const Json::Value& json, const SendMessageTarget& target);
};
