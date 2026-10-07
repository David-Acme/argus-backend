#pragma once

#include <cstdint>
#include <string>

enum class ReminderCallOutcome : unsigned char
{
  Scheduled,
  TooFar,
  InThePast,
  TooMany,
  Unavailable,
  Refused
};

struct ReminderCallRequest
{
  int64_t userId{0};
  int64_t fireAt{0};
  std::string topic;
  std::string lang;
  std::string commandId;
};

class ReminderCallScheduler
{
public:
  virtual ~ReminderCallScheduler() = default;

  [[nodiscard]] virtual ReminderCallOutcome schedule(const ReminderCallRequest& request) const = 0;
};
