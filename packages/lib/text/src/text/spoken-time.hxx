#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace spoken_time
{

enum class Day : unsigned char
{
  Relative,
  Weekday
};

struct When
{
  int64_t epoch{0};
  int64_t now{0};
  std::string_view lang{};
  Day day{Day::Relative};
  bool bare{false};
};

[[nodiscard]] std::string weekdayName(const When& when);

[[nodiscard]] std::string weekdayDate(const When& when);

[[nodiscard]] std::string day(const When& when);

[[nodiscard]] std::string clock(const When& when);

[[nodiscard]] std::string moment(const When& when);

}
