#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct CallTimeInput
{
  std::string_view text;
  std::string_view lang;
  int64_t now{0};
};

struct CallTime
{
  int64_t fireAt{0};
  std::size_t phraseBegin{0};
  std::size_t phraseEnd{0};
};

struct DayConflict
{
  int64_t byWeekday{0};
  int64_t byDate{0};
  std::size_t phraseBegin{0};
  std::size_t phraseEnd{0};
  int relative{-1};
};

struct CallReading
{
  std::optional<CallTime> time{};
  std::optional<DayConflict> conflict{};
  bool farAway{false};
};

namespace call_time
{
[[nodiscard]] CallReading read(const CallTimeInput& input);

[[nodiscard]] std::optional<CallTime> resolve(const CallTimeInput& input);

[[nodiscard]] std::string withoutPhrase(std::string_view text, const CallTime& time);
}
