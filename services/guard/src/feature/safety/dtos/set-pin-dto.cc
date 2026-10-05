#include "set-pin-dto.hxx"

#include <validation/validation_dsl.hxx>

SetPinDto SetPinDto::fromJson(const Json::Value& json)
{
  SetPinDto dto;
  dto.disarmPin = json.get("disarmPin", "").asString();
  dto.duressPin = json.get("duressPin", "").asString();

  START_VALIDATION(SetPinDto, dto)
  IS_NOT_EMPTY(disarmPin)
  MATCHES_REGEX(disarmPin, "^[0-9]{4,8}$", "must be 4 to 8 digits")
  IS_NOT_EMPTY(duressPin)
  MATCHES_REGEX(duressPin, "^[0-9]{4,8}$", "must be 4 to 8 digits")
  CUSTOM_LAMBDA(duressPin, [](const SetPinDto& value) -> std::optional<std::string> {
    if (!value.duressPin.empty() && value.duressPin == value.disarmPin)
      return "must differ from disarmPin";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
