#include "create-calendar-event-dto.hxx"

CreateCalendarEventDto CreateCalendarEventDto::fromJson(const Json::Value& json)
{
  CreateCalendarEventDto dto;
  dto.title = json.get("title", "").asString();
  dto.description = json.get("description", "").asString();
  dto.location = json.get("location", "").asString();
  dto.color = json.get("color", "").asString();
  if (json.isMember("startsAt") && json["startsAt"].isInt64())
    dto.startsAt = json["startsAt"].asInt64();
  if (json.isMember("endsAt") && json["endsAt"].isInt64())
    dto.endsAt = json["endsAt"].asInt64();
  dto.isAllDay = json.get("isAllDay", false).asBool();
  if (json.isMember("recurrenceRule") && json["recurrenceRule"].isString())
    dto.recurrenceRule = json["recurrenceRule"].asString();
  if (json.isMember("projectId") && json["projectId"].isInt64())
    dto.projectId = json["projectId"].asInt64();

  START_VALIDATION(CreateCalendarEventDto, dto)
  IS_NOT_EMPTY(title)
  MAX_LENGTH(title, 200)
  MAX_LENGTH(description, 2000)
  MAX_LENGTH(location, 200)
  IS_POSITIVE_TIMESTAMP(startsAt)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(endsAt)
  IS_BOOLEAN(isAllDay)
  CUSTOM_LAMBDA(endsAt,
                [](const CreateCalendarEventDto& d)
                    -> std::optional<std::string> {
                  if (d.endsAt && *d.endsAt < d.startsAt)
                    return "endsAt must not be before startsAt";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}
