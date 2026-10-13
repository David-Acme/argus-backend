#include "screen-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <cstddef>

namespace
{
constexpr std::size_t kMaxTextLength = std::size_t{32} * 1024;
}

ScreenDto ScreenDto::fromJson(const Json::Value& json)
{
  ScreenDto dto;
  if (json.isMember("text") && json["text"].isString())
    dto.text = json["text"].asString();
  if (json.isMember("user_id") && json["user_id"].isIntegral())
    dto.userId = json["user_id"].asInt64();
  if (json.isMember("role") && json["role"].isString())
    dto.role = json["role"].asString();

  START_VALIDATION(ScreenDto, dto)
  IS_NOT_EMPTY(text)
  MAX_LENGTH(text, kMaxTextLength)
  CUSTOM_LAMBDA(userId, [](const ScreenDto& value) -> std::optional<std::string> {
    if (value.userId && *value.userId < 0)
      return "user_id must not be negative";
    return std::nullopt;
  })
  IS_IN_OPTIONAL(role, "owner", "resident", "guard", "guest")
  END_VALIDATION()
  return dto;
}

ChatRequest ScreenDto::request() const
{
  ChatRequest request;
  request.messages = {{.role = "user", .content = text}};
  request.toolsEnabled = false;
  request.userId = userId.value_or(0);
  request.role = role ? userRoleFromString(*role) : UserRole::Unknown;
  return request;
}
