#include "create-reminder-dto.hxx"

#include <utility>

CreateReminderDto CreateReminderDto::validated(CreateReminderDto dto)
{
  START_VALIDATION(CreateReminderDto, dto)
  IS_NOT_EMPTY(title)
  MAX_LENGTH(title, 200)
  MAX_LENGTH(description, 2000)
  MAX_LENGTH_OPTIONAL(recurrenceRule, 512)
  IS_POSITIVE_TIMESTAMP(scheduledAt)
  END_VALIDATION()
  return dto;
}

CreateReminderDto CreateReminderDto::fromJson(const Json::Value& json)
{
  CreateReminderDto dto;
  dto.title = json.get("title", "").asString();
  dto.description = json.get("description", "").asString();
  if (json.isMember("scheduledAt") && json["scheduledAt"].isInt64())
    dto.scheduledAt = json["scheduledAt"].asInt64();
  if (json.isMember("recurrenceRule") && json["recurrenceRule"].isString())
    dto.recurrenceRule = json["recurrenceRule"].asString();
  return validated(std::move(dto));
}
