#include "send-message-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <algorithm>
#include <optional>
#include <string_view>

namespace
{
constexpr std::string_view kSpace = " \t\r\n";

std::string trimmed(const std::string& text)
{
  const auto first = text.find_first_not_of(kSpace);
  if (first == std::string::npos)
    return {};
  const auto last = text.find_last_not_of(kSpace);
  return text.substr(first, last - first + 1);
}

int64_t characterCount(std::string_view text)
{
  return static_cast<int64_t>(
      std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}
}

SendMessageDto SendMessageDto::fromJson(const Json::Value& json, const SendMessageTarget& target)
{
  SendMessageDto dto;
  dto.counterpartId = target.counterpartId;
  if (json.isMember("body") && json["body"].isString())
    dto.body = trimmed(json["body"].asString());
  const int64_t maxChars = target.maxChars;

  START_VALIDATION(SendMessageDto, dto)
  IS_POSITIVE(counterpartId)
  IS_NOT_EMPTY(body)
  CUSTOM_LAMBDA(body, [maxChars](const SendMessageDto& value) -> std::optional<std::string> {
    if (characterCount(value.body) <= maxChars)
      return std::nullopt;
    return "body must be at most " + std::to_string(maxChars) + " characters";
  })
  END_VALIDATION()
  return dto;
}
