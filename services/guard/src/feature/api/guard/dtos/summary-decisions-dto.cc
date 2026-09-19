#include "summary-decisions-dto.hxx"

#include <charconv>
#include <shared/validation/validation_dsl.hxx>
#include <string>
#include <system_error>

namespace
{
int64_t parseBound(const std::string& value)
{
  int64_t parsed = 0;
  if (!value.empty()) {
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size())
      parsed = -1;
  }
  return parsed;
}

int parseMargin(const std::string& value)
{
  int parsed = 0;
  if (!value.empty()) {
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size())
      parsed = -1;
  }
  return parsed;
}
} // namespace

SummaryDecisionsDto SummaryDecisionsDto::fromRequest(
    const drogon::HttpRequestPtr& request)
{
  SummaryDecisionsDto dto;
  dto.from = parseBound(request->getParameter("from"));
  dto.to = parseBound(request->getParameter("to"));
  dto.nearMissMargin = parseMargin(request->getParameter("near_miss_margin"));

  START_VALIDATION(SummaryDecisionsDto, dto)
  IS_NON_NEGATIVE(from)
  IS_NON_NEGATIVE(to)
  IS_NON_NEGATIVE(nearMissMargin)
  END_VALIDATION()
  return dto;
}
