#include "delivery-summary-dto.hxx"

#include <charconv>
#include <shared/validation/validation_dsl.hxx>
#include <string>
#include <system_error>

DeliverySummaryDto DeliverySummaryDto::fromRequest(
    const drogon::HttpRequestPtr& request)
{
  DeliverySummaryDto dto;
  const std::string value = request->getParameter("since");
  if (!value.empty()) {
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), dto.since);
    if (error != std::errc{} || end != value.data() + value.size())
      dto.since = -1;
  }

  START_VALIDATION(DeliverySummaryDto, dto)
  IS_NON_NEGATIVE(since)
  END_VALIDATION()
  return dto;
}
