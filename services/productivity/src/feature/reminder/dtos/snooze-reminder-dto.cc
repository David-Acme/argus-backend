#include "snooze-reminder-dto.hxx"

SnoozeReminderDto SnoozeReminderDto::validated(SnoozeReminderDto dto)
{
  START_VALIDATION(SnoozeReminderDto, dto)
  CUSTOM_LAMBDA(minutes, [](const SnoozeReminderDto& value) -> std::optional<std::string> {
    if (value.tomorrow == value.minutes.has_value())
      return "snooze needs exactly one of minutes or tomorrow";
    if (value.minutes && (*value.minutes < 5 || *value.minutes > 1440))
      return "minutes must be between 5 and 1440";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}

SnoozeReminderDto SnoozeReminderDto::fromJson(const Json::Value& json)
{
  SnoozeReminderDto dto;
  if (json.isMember("minutes") && json["minutes"].isInt64())
    dto.minutes = json["minutes"].asInt64();
  if (json.isMember("tomorrow") && json["tomorrow"].isBool())
    dto.tomorrow = json["tomorrow"].asBool();
  return validated(dto);
}
