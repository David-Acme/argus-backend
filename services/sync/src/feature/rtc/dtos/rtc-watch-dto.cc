#include "rtc-watch-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <algorithm>

namespace
{
constexpr size_t kMaxWatchReplyBytes = 4000;
}

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

RtcWatchReplyDto RtcWatchReplyDto::fromJson(const Json::Value& json, const std::string& id)
{
  RtcWatchReplyDto dto;
  dto.conversationId = id;
  if (json.isMember("text") && json["text"].isString())
    dto.text = json["text"].asString();
  START_VALIDATION(RtcWatchReplyDto, dto)
  CUSTOM_LAMBDA(conversationId, [](const RtcWatchReplyDto& value) -> std::optional<std::string> {
    if (isWatchConversationId(value.conversationId))
      return std::nullopt;
    return "conversationId must be 32 lowercase hex characters";
  })
  IS_NOT_EMPTY(text)
  MAX_LENGTH(text, kMaxWatchReplyBytes)
  END_VALIDATION()
  return dto;
}
