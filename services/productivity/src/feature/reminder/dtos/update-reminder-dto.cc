#include "update-reminder-dto.hxx"

#include <shared/services/recurrence/recurrence.hxx>
#include <utility>

UpdateReminderDto UpdateReminderDto::validated(UpdateReminderDto dto)
{
  START_VALIDATION(UpdateReminderDto, dto)
  IS_NOT_EMPTY_OPTIONAL(title)
  MAX_LENGTH_OPTIONAL(title, 200)
  MAX_LENGTH_OPTIONAL(description, 2000)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(scheduledAt)
  MAX_LENGTH_OPTIONAL(recurrenceRule, 512)
  CUSTOM_LAMBDA(recurrenceRule, [](const UpdateReminderDto& value)
                -> std::optional<std::string> {
    if (value.recurrenceRule && !value.recurrenceRule->empty() &&
        !parseRecurrenceRule(*value.recurrenceRule))
      return std::string(kRecurrenceRuleProblem);
    return std::nullopt;
  })
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
  if (json.isMember("recurrenceRule") && json["recurrenceRule"].isString())
    dto.recurrenceRule = json["recurrenceRule"].asString();
  else if (json.isMember("recurrenceRule") && json["recurrenceRule"].isNull())
    dto.recurrenceRule = std::string{};
  return validated(std::move(dto));
}
