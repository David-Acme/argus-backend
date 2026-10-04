#include "list-episodes-dto.hxx"

#include <charconv>
#include <string>
#include <system_error>
#include <validation/validation_dsl.hxx>

namespace
{
constexpr int kMaxEpisodeLimit = 100;

template <typename Number>
Number parsed(const std::string& value, Number fallback, Number invalid)
{
  if (value.empty())
    return fallback;
  Number result{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size())
    return invalid;
  return result;
}
}

ListEpisodesDto ListEpisodesDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  ListEpisodesDto dto;
  dto.limit = parsed<int>(request->getParameter("limit"), 30, 0);
  dto.before = parsed<int64_t>(request->getParameter("before"), 0, -1);
  dto.environmentId =
      parsed<int64_t>(request->getParameter("environmentId"), 0, -1);
  if (dto.limit > kMaxEpisodeLimit)
    dto.limit = kMaxEpisodeLimit;

  START_VALIDATION(ListEpisodesDto, dto)
  IS_POSITIVE(limit)
  IS_NON_NEGATIVE(before)
  IS_NON_NEGATIVE(environmentId)
  END_VALIDATION()
  return dto;
}
