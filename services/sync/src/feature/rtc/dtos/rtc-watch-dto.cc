#include "rtc-watch-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <algorithm>

bool isWatchConversationId(const std::string& value)
{
  constexpr size_t kIdChars = 32;
  return value.size() == kIdChars &&
         std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

RtcWatchEndDto RtcWatchEndDto::fromPath(const std::string& id)
{
  RtcWatchEndDto dto;
  dto.conversationId = id;
  START_VALIDATION(RtcWatchEndDto, dto)
  CUSTOM_LAMBDA(conversationId, [](const RtcWatchEndDto& value) -> std::optional<std::string> {
    if (isWatchConversationId(value.conversationId))
      return std::nullopt;
    return "conversationId must be 32 lowercase hex characters";
  })
  END_VALIDATION()
  return dto;
}
