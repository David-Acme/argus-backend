#include "notification-ack-dto.hxx"

NotificationAckDto NotificationAckDto::fromJson(const Json::Value& json)
{
  NotificationAckDto dto;
  const auto& ids = json["notification_ids"];
  if (ids.isArray()) {
    dto.notificationIds.reserve(ids.size());
    for (const auto& id : ids) {
      if (id.isInt64())
        dto.notificationIds.push_back(id.asInt64());
      else if (id.isUInt64() && id.asUInt64() <= INT64_MAX)
        dto.notificationIds.push_back(static_cast<int64_t>(id.asUInt64()));
    }
  }

  START_VALIDATION(NotificationAckDto, dto)
  ARRAY_NOT_EMPTY(notificationIds, int64_t)
  MIN_ELEMENTS(notificationIds, int64_t, 1)
  END_VALIDATION()
  for (const int64_t id : dto.notificationIds) {
    if (id <= 0)
      throw ValidationException({{"notification_ids", {"ids must be positive"}}},
                                422);
  }

  return dto;
}
