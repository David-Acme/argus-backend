#include "list-visitors-dto.hxx"

#include <charconv>
#include <system_error>
#include <utility>
#include <validation/validation_dsl.hxx>

namespace
{
constexpr int64_t kDefaultLimit = 500;
constexpr int64_t kMaxSearchLength = 64;

int64_t numberOf(const std::string& value, int64_t fallback)
{
  if (value.empty())
    return fallback;
  int64_t result{0};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size())
    return -1;
  return result;
}

std::string orDefault(std::string value, const char* fallback)
{
  return value.empty() ? std::string(fallback) : std::move(value);
}
}

ListVisitorsDto ListVisitorsDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  ListVisitorsDto dto;
  dto.scope = orDefault(request->getParameter("scope"), "all");
  dto.filter = orDefault(request->getParameter("filter"), "all");
  dto.search = request->getParameter("q");
  dto.limit = numberOf(request->getParameter("limit"), kDefaultLimit);
  dto.beforeSeen = numberOf(request->getParameter("beforeSeen"), 0);
  dto.beforeId = numberOf(request->getParameter("beforeId"), 0);
  START_VALIDATION(ListVisitorsDto, dto)
  IS_IN(scope, "all", "named")
  IS_IN(filter, "all", "named", "unnamed", "watchlist")
  MAX_LENGTH(search, kMaxSearchLength)
  BETWEEN(limit, 1, kDefaultLimit)
  IS_NON_NEGATIVE(beforeSeen)
  IS_NON_NEGATIVE(beforeId)
  END_VALIDATION()
  return dto;
}
