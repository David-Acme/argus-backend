#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class FeedbackLabel : uint8_t
{
  Useful = 0,
  FalseAlarm,
  NotNow
};

inline std::string feedbackLabelToString(FeedbackLabel label)
{
  switch (label) {
    case FeedbackLabel::Useful:
      return "useful";
    case FeedbackLabel::FalseAlarm:
      return "false_alarm";
    case FeedbackLabel::NotNow:
      return "not_now";
  }
  return "useful";
}

inline std::optional<FeedbackLabel>
feedbackLabelFromString(const std::string& value)
{
  if (value == "useful")
    return FeedbackLabel::Useful;
  if (value == "false_alarm")
    return FeedbackLabel::FalseAlarm;
  if (value == "not_now")
    return FeedbackLabel::NotNow;
  return std::nullopt;
}
