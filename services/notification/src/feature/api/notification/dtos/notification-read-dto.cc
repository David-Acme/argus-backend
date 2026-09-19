#include "notification-read-dto.hxx"

NotificationReadDto NotificationReadDto::fromJson(const Json::Value& json)
{
  NotificationReadDto dto;
  const auto& ids = json["ids"];
  if (ids.isArray()) {
    dto.ids.reserve(ids.size());
    for (const auto& id : ids) {
      if (id.isInt64())
        dto.ids.push_back(id.asInt64());
      else if (id.isUInt64() && id.asUInt64() <= INT64_MAX)
        dto.ids.push_back(static_cast<int64_t>(id.asUInt64()));
    }
  }

  START_VALIDATION(NotificationReadDto, dto)
  ARRAY_NOT_EMPTY(ids, int64_t)
  MIN_ELEMENTS(ids, int64_t, 1)
  END_VALIDATION()

  return dto;
}
