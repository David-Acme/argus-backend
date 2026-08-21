#include "update-calendar-event-share-dto.hxx"

UpdateCalendarEventShareDto UpdateCalendarEventShareDto::fromJson(const Json::Value& json)
{
  UpdateCalendarEventShareDto dto;
  dto.access = json.get("access", "").asString();

  START_VALIDATION(UpdateCalendarEventShareDto, dto)
  IS_IN(access, "view", "edit")
  END_VALIDATION()
  return dto;
}
