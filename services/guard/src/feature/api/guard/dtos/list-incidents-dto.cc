#include "list-incidents-dto.hxx"

#include <charconv>
#include <shared/validation/validation_dsl.hxx>
#include <string>
#include <system_error>

namespace
{
constexpr int kMaxIncidentLimit = 200;
} // namespace

ListIncidentsDto ListIncidentsDto::fromRequest(
    const drogon::HttpRequestPtr& request)
{
  ListIncidentsDto dto;
  const std::string value = request->getParameter("limit");
  if (!value.empty()) {
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), dto.limit);
    if (error != std::errc{} || end != value.data() + value.size())
      dto.limit = 0;
  }
  if (dto.limit > kMaxIncidentLimit)
    dto.limit = kMaxIncidentLimit;

  START_VALIDATION(ListIncidentsDto, dto)
  IS_POSITIVE(limit)
  END_VALIDATION()
  return dto;
}
