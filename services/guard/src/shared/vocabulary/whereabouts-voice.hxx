#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <json/value.h>
#include <string>

struct WhereaboutsVoiceRequest
{
  int64_t userId{0};
  std::string role;
  std::string lang;
  std::string person;
  std::string room;
};

struct WhereaboutsVoiceReply
{
  bool ok{false};
  std::string text;
  std::string code;
  Json::Value structured{Json::objectValue};
};

using WhereaboutsVoice = std::function<drogon::Task<WhereaboutsVoiceReply>(WhereaboutsVoiceRequest)>;
