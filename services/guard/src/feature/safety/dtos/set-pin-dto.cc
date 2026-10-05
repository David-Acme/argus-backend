#include "set-pin-dto.hxx"

#include <feature/safety/services/pin-hash.hxx>

#include <validation/validation_dsl.hxx>

SetPinDto SetPinDto::fromJson(const Json::Value& json)
{
  SetPinDto dto;
  dto.disarmPin = json.get("disarmPin", "").asString();
  dto.duressPin = json.get("duressPin", "").asString();
  const bool hasCurrent = json.isMember("currentPin") && !json["currentPin"].isNull();
  if (hasCurrent && json["currentPin"].isString())
    dto.currentPin = json["currentPin"].asString();

  START_VALIDATION(SetPinDto, dto)
  IS_NOT_EMPTY(disarmPin)
  MATCHES_REGEX(disarmPin, "^[0-9]{4,8}$", "must be 4 to 8 digits")
  IS_NOT_EMPTY(duressPin)
  MATCHES_REGEX(duressPin, "^[0-9]{4,8}$", "must be 4 to 8 digits")
  CUSTOM_LAMBDA(disarmPin, [](const SetPinDto& value) -> std::optional<std::string> {
    if (pin_hash::wellFormedPin(value.disarmPin) && pin_hash::trivialPin(value.disarmPin))
      return "is too easy to guess";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(duressPin, [](const SetPinDto& value) -> std::optional<std::string> {
    if (!value.duressPin.empty() && value.duressPin == value.disarmPin)
      return "must differ from disarmPin";
    if (pin_hash::wellFormedPin(value.duressPin) && pin_hash::trivialPin(value.duressPin))
      return "is too easy to guess";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(currentPin, [hasCurrent](const SetPinDto& value) -> std::optional<std::string> {
    if (hasCurrent && (!value.currentPin || !pin_hash::wellFormedPin(*value.currentPin)))
      return "must be 4 to 8 digits";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
