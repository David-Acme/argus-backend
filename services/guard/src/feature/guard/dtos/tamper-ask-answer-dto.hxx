#pragma once

#include <json/value.h>
#include <string>

struct TamperAskAnswerDto
{
  std::string answer;

  static TamperAskAnswerDto fromJson(const Json::Value& json);
};
