#pragma once

#include <feature/memory/services/extract/repeat-rule.hxx>
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

struct RepeatSpoken
{
  const Repeat& repeat;
  int64_t firstAt{0};
  int64_t now{0};
  std::string_view lang;
  bool listed{true};
};

[[nodiscard]] std::string repeatMoment(const RepeatSpoken& spoken);

[[nodiscard]] std::string repeatSentence(const RepeatSpoken& spoken);

[[nodiscard]] std::string untimedRepeat(std::string_view lang);

[[nodiscard]] std::string unreadableRepeat(std::string_view lang);

}
