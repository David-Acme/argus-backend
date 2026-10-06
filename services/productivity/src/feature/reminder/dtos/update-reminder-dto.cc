#include "update-reminder-dto.hxx"

#include <utility>

UpdateReminderDto UpdateReminderDto::validated(UpdateReminderDto dto)
{
  START_VALIDATION(UpdateReminderDto, dto)
  IS_NOT_EMPTY_OPTIONAL(title)
  MAX_LENGTH_OPTIONAL(title, 200)
  MAX_LENGTH_OPTIONAL(description, 2000)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(scheduledAt)
  END_VALIDATION()
  return dto;
}

UpdateReminderDto UpdateReminderDto::fromJson(const Json::Value& json)
{
  UpdateReminderDto dto;
  if (json.isMember("title") && json["title"].isString())
    dto.title = json["title"].asString();
  if (json.isMember("description") && json["description"].isString())
    dto.description = json["description"].asString();
  else if (json.isMember("description") && json["description"].isNull())
    dto.description = std::string{};
  if (json.isMember("scheduledAt") && json["scheduledAt"].isInt64())
    dto.scheduledAt = json["scheduledAt"].asInt64();
  if (json.isMember("isCompleted") && json["isCompleted"].isBool())
    dto.isCompleted = json["isCompleted"].asBool();
  return validated(std::move(dto));
}
