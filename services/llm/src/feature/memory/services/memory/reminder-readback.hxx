#pragma once

#include <feature/memory/services/memory/reminder-call-scheduler.hxx>

#include <cstdint>
#include <string>
#include <string_view>

namespace reminder_readback
{

enum class Call : unsigned char
{
  Scheduled,
  NotAttempted,
  Failed
};

struct Spoken
{
  int64_t fireAt{0};
  int64_t now{0};
  std::string_view lang;
  Call call{Call::NotAttempted};
  bool listed{true};
  ReminderCallOutcome why{ReminderCallOutcome::Refused};
};

[[nodiscard]] std::string moment(const Spoken& spoken);

[[nodiscard]] std::string sentence(const Spoken& spoken);

}
