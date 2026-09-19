#pragma once

#include <json/value.h>
#include <string>

// Resident label on one decision-journal row, collected for offline
// calibration only: {"label": "useful|false_alarm|not_now"}. Labels never
// retune live thresholds.
struct FeedbackDecisionDto
{
  std::string label;

  static FeedbackDecisionDto fromJson(const Json::Value& json);
};
