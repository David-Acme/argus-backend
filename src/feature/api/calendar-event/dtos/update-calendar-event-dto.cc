#include "update-calendar-event-dto.hxx"

UpdateCalendarEventDto UpdateCalendarEventDto::fromJson(const Json::Value& json)
{
  UpdateCalendarEventDto dto;
  if (json.isMember("title") && json["title"].isString())
    dto.title = json["title"].asString();
  if (json.isMember("description") && json["description"].isString())
    dto.description = json["description"].asString();
  if (json.isMember("location") && json["location"].isString())
    dto.location = json["location"].asString();
  if (json.isMember("color") && json["color"].isString())
    dto.color = json["color"].asString();
  if (json.isMember("startsAt") && json["startsAt"].isInt64())
    dto.startsAt = json["startsAt"].asInt64();
  if (json.isMember("endsAt") && json["endsAt"].isInt64())
    dto.endsAt = json["endsAt"].asInt64();
  if (json.isMember("isAllDay") && json["isAllDay"].isBool())
    dto.isAllDay = json["isAllDay"].asBool();
  if (json.isMember("recurrenceRule") && json["recurrenceRule"].isString())
    dto.recurrenceRule = json["recurrenceRule"].asString();
  if (json.isMember("projectId") && json["projectId"].isInt64())
    dto.projectId = json["projectId"].asInt64();

  START_VALIDATION(UpdateCalendarEventDto, dto)
  IS_NOT_EMPTY_OPTIONAL(title)
  MAX_LENGTH_OPTIONAL(title, 200)
  MAX_LENGTH_OPTIONAL(description, 2000)
  MAX_LENGTH_OPTIONAL(location, 200)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(startsAt)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(endsAt)
  END_VALIDATION()
  return dto;
}
