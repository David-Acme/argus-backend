#pragma once

#include <json/value.h>
#include <string>

struct FeedbackDecisionDto
{
  std::string label;

  static FeedbackDecisionDto fromJson(const Json::Value& json);
};
