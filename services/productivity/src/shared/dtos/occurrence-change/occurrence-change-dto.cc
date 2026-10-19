#include "occurrence-change-dto.hxx"

#include <utility>

bool OccurrenceChangeDto::changesMoreThanTheTime() const
{
  return endsAt.has_value() || title.has_value() || location.has_value();
}

OccurrenceChangeDto OccurrenceChangeDto::validated(OccurrenceChangeDto dto)
{
  START_VALIDATION(OccurrenceChangeDto, dto)
  IS_POSITIVE_TIMESTAMP(occurrence)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(startsAt)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(endsAt)
  IS_NOT_EMPTY_OPTIONAL(title)
  MAX_LENGTH_OPTIONAL(title, 200)
  MAX_LENGTH_OPTIONAL(location, 200)
  CUSTOM_LAMBDA(cancelled, [](const OccurrenceChangeDto& value) -> std::optional<std::string> {
    if (value.cancelled && (value.startsAt || value.changesMoreThanTheTime()))
      return "a cancelled occurrence carries no other change";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(endsAt, [](const OccurrenceChangeDto& value) -> std::optional<std::string> {
    if (value.endsAt && *value.endsAt < value.startsAt.value_or(value.occurrence))
      return "endsAt must not be before the occurrence starts";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}

OccurrenceChangeDto OccurrenceChangeDto::fromJson(const Json::Value& json)
{
  OccurrenceChangeDto dto;
  if (json.isMember("occurrence") && json["occurrence"].isInt64())
    dto.occurrence = json["occurrence"].asInt64();
  if (json.isMember("cancelled") && json["cancelled"].isBool())
    dto.cancelled = json["cancelled"].asBool();
  if (json.isMember("startsAt") && json["startsAt"].isInt64())
    dto.startsAt = json["startsAt"].asInt64();
  if (json.isMember("endsAt") && json["endsAt"].isInt64())
    dto.endsAt = json["endsAt"].asInt64();
  if (json.isMember("title") && json["title"].isString())
    dto.title = json["title"].asString();
  if (json.isMember("location") && json["location"].isString())
    dto.location = json["location"].asString();
  return validated(std::move(dto));
}
