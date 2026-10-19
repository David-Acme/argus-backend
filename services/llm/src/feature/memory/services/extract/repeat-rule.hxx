#pragma once

#include <feature/memory/services/extract/call-time.hxx>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class RepeatFrequency : std::uint8_t
{
  Daily,
  Weekly,
  Monthly,
  Yearly
};

struct Repeat
{
  RepeatFrequency frequency{RepeatFrequency::Daily};
  int interval{1};
  std::vector<int> weekdays{};
  int monthDay{0};

  bool operator==(const Repeat& other) const = default;
};

struct RepeatHeard
{
  std::string_view text;
  std::string_view lang;
};

struct RepeatReading
{
  std::optional<Repeat> repeat{};
  bool mentioned{false};
  DayPart part{DayPart::None};
  std::size_t phraseBegin{0};
  std::size_t phraseEnd{0};
};

struct FirstOccurrences
{
  const Repeat& repeat;
  int minuteOfDay{0};
  int64_t now{0};
};

namespace repeat_rule
{
inline constexpr int kMaxInterval = 999;

[[nodiscard]] RepeatReading read(const RepeatHeard& heard);

[[nodiscard]] std::string format(const Repeat& repeat);

[[nodiscard]] std::optional<Repeat> parse(std::string_view text);

[[nodiscard]] std::string productivityRule(const Repeat& repeat);

[[nodiscard]] std::vector<int64_t> firstOccurrences(const FirstOccurrences& input);

[[nodiscard]] bool namesMonth(std::string_view text);
}
