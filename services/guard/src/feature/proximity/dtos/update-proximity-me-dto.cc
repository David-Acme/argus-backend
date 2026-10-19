#include "update-proximity-me-dto.hxx"

#include <validation/validation_dsl.hxx>

#include <optional>
#include <string>

UpdateProximityMeDto UpdateProximityMeDto::fromJson(const Json::Value& json)
{
  UpdateProximityMeDto dto;
  const bool given = json.isMember("ble") && json["ble"].isBool();
  dto.ble = given && json["ble"].asBool();

  START_VALIDATION(UpdateProximityMeDto, dto)
  CUSTOM_LAMBDA(ble, [given](const UpdateProximityMeDto&) -> std::optional<std::string> {
    if (!given)
      return "must be a boolean";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
