#include "create-expected-guest-dto.hxx"

#include <shared/validation/validation_dsl.hxx>

namespace
{
constexpr int kMaxDescriptionLength = 200;
constexpr int kMaxWindowHours = 24;
} // namespace

CreateExpectedGuestDto CreateExpectedGuestDto::fromJson(
    const Json::Value& json)
{
  CreateExpectedGuestDto dto;
  dto.description = json.get("description", "").asString();
  if (json.isMember("cameraId") && json["cameraId"].isInt64())
    dto.cameraId = json["cameraId"].asInt64();
  if (json.isMember("personId") && json["personId"].isInt64())
    dto.personId = json["personId"].asInt64();
  if (json.isMember("hostUserId") && json["hostUserId"].isInt64())
    dto.hostUserId = json["hostUserId"].asInt64();
  if (json.isMember("oneTime") && json["oneTime"].isBool())
    dto.oneTime = json["oneTime"].asBool();
  if (json.isMember("validFrom") && json["validFrom"].isInt64())
    dto.validFrom = json["validFrom"].asInt64();
  if (json.isMember("validUntil") && json["validUntil"].isInt64())
    dto.validUntil = json["validUntil"].asInt64();
  if (json.isMember("hours") && json["hours"].isInt())
    dto.hours = json["hours"].asInt();

  START_VALIDATION(CreateExpectedGuestDto, dto)
  IS_NOT_EMPTY(description)
  MAX_LENGTH(description, kMaxDescriptionLength)
  IS_NON_NEGATIVE(cameraId)
  IS_NON_NEGATIVE(personId)
  IS_NON_NEGATIVE(hostUserId)
  BETWEEN(hours, 1, kMaxWindowHours)
  CUSTOM_LAMBDA(validFrom,
                [](const CreateExpectedGuestDto& value)
                    -> std::optional<std::string> {
                  if (value.validFrom < 0 || value.validUntil < 0)
                    return "timestamps must not be negative";
                  if (value.validUntil > 0 && value.validFrom > 0 &&
                      value.validUntil <= value.validFrom)
                    return "validUntil must be after validFrom";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}
