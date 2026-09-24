#include "create-calendar-event-share-dto.hxx"

CreateCalendarEventShareDto CreateCalendarEventShareDto::fromJson(const Json::Value& json)
{
  CreateCalendarEventShareDto dto;
  if (json.isMember("calendarEventId") && json["calendarEventId"].isInt64())
    dto.calendarEventId = json["calendarEventId"].asInt64();
  if (json.isMember("userId") && json["userId"].isInt64())
    dto.userId = json["userId"].asInt64();
  dto.access = json.get("access", "view").asString();

  START_VALIDATION(CreateCalendarEventShareDto, dto)
  IS_POSITIVE(calendarEventId)
  IS_POSITIVE(userId)
  IS_IN(access, "view", "edit")
  END_VALIDATION()
  return dto;
}
