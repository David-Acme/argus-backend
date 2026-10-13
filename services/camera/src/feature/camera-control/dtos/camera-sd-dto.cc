#include "camera-sd-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <charconv>
#include <chrono>
#include <optional>
#include <string_view>

namespace
{

constexpr int kMaxSpanDays = 366;

template <typename Number>
std::optional<Number> numberOf(std::string_view text)
{
  Number value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return value;
}

std::optional<std::chrono::sys_days> isoDay(std::string_view text)
{
  if (text.size() != 10 || text[4] != '-' || text[7] != '-')
    return std::nullopt;
  const auto year = numberOf<int>(text.substr(0, 4));
  const auto month = numberOf<unsigned>(text.substr(5, 2));
  const auto day = numberOf<unsigned>(text.substr(8, 2));
  if (!year || !month || !day)
    return std::nullopt;
  const std::chrono::year_month_day date{std::chrono::year{*year}, std::chrono::month{*month},
                                         std::chrono::day{*day}};
  if (!date.ok())
    return std::nullopt;
  return std::chrono::sys_days{date};
}

}

CameraSdDaysDto CameraSdDaysDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  CameraSdDaysDto dto;
  dto.from = request->getParameter("from");
  dto.to = request->getParameter("to");

  START_VALIDATION(CameraSdDaysDto, dto)
  CUSTOM_LAMBDA(from, [](const CameraSdDaysDto& value) -> std::optional<std::string> {
    if (!isoDay(value.from))
      return "from is a date written YYYY-MM-DD";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(to, [](const CameraSdDaysDto& value) -> std::optional<std::string> {
    const auto from = isoDay(value.from);
    const auto to = isoDay(value.to);
    if (!to)
      return "to is a date written YYYY-MM-DD";
    if (from && *to < *from)
      return "to is not before from";
    if (from && *to - *from > std::chrono::days{kMaxSpanDays})
      return "a search spans at most 366 days";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}

CameraSdDayDto CameraSdDayDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  CameraSdDayDto dto;
  dto.date = request->getParameter("date");

  START_VALIDATION(CameraSdDayDto, dto)
  CUSTOM_LAMBDA(date, [](const CameraSdDayDto& value) -> std::optional<std::string> {
    if (!isoDay(value.date))
      return "date is a date written YYYY-MM-DD";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
