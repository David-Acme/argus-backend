#include "remove-pin-dto.hxx"

#include <feature/safety/safety-errors.hxx>
#include <feature/safety/services/pin-hash.hxx>

#include <errors/response-exception.hxx>

#include <validation/validation_dsl.hxx>

RemovePinDto RemovePinDto::fromRequest(const drogon::HttpRequestPtr& req)
{
  RemovePinDto dto;
  const auto json = req->getJsonObject();
  if (!req->body().empty() && (!json || !json->isObject()))
    throw ResponseException(SafetyErrors::MalformedBody);
  const bool hasCurrent =
      json && json->isObject() && json->isMember("currentPin") && !(*json)["currentPin"].isNull();
  if (hasCurrent && (*json)["currentPin"].isString())
    dto.currentPin = (*json)["currentPin"].asString();

  START_VALIDATION(RemovePinDto, dto)
  CUSTOM_LAMBDA(currentPin, [hasCurrent](const RemovePinDto& value) -> std::optional<std::string> {
    if (hasCurrent && (!value.currentPin || !pin_hash::wellFormedPin(*value.currentPin)))
      return "must be 4 to 8 digits";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
