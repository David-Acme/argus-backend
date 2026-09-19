#include "remove-expected-guest-dto.hxx"

#include <charconv>
#include <shared/validation/validation_dsl.hxx>
#include <string>
#include <system_error>

RemoveExpectedGuestDto RemoveExpectedGuestDto::fromRequest(
    const drogon::HttpRequestPtr& request)
{
  RemoveExpectedGuestDto dto;
  const std::string value = request->getParameter("id");
  if (!value.empty()) {
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), dto.id);
    if (error != std::errc{} || end != value.data() + value.size())
      dto.id = 0;
  }

  START_VALIDATION(RemoveExpectedGuestDto, dto)
  IS_POSITIVE(id)
  END_VALIDATION()
  return dto;
}
