#include "idempotency-key-dto.hxx"

#include <algorithm>
#include <cctype>
#include <optional>

namespace
{
bool keyCharacter(char value)
{
  return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '-' || value == '_';
}
}

IdempotencyKeyDto IdempotencyKeyDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  IdempotencyKeyDto dto;
  dto.key = request->getHeader(std::string(kHeader));

  START_VALIDATION(IdempotencyKeyDto, dto)
  MAX_LENGTH(key, 64)
  CUSTOM_LAMBDA(key, [](const IdempotencyKeyDto& d) -> std::optional<std::string> {
    if (std::ranges::all_of(d.key, keyCharacter))
      return std::nullopt;
    return "Idempotency-Key may only hold letters, digits, '-' and '_'";
  })
  END_VALIDATION()
  return dto;
}
