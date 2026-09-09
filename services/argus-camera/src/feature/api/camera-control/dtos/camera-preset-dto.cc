#include "camera-preset-dto.hxx"

CameraPresetDto CameraPresetDto::fromJson(const Json::Value& json)
{
  CameraPresetDto dto;
  dto.action = json.get("action", "goto").asString();
  dto.id = json.get("id", "").asString();
  dto.name = json.get("name", "").asString();

  START_VALIDATION(CameraPresetDto, dto)
  IS_IN(action, "goto", "save", "delete")
  MAX_LENGTH(id, 32)
  MAX_LENGTH(name, 64)
  CUSTOM_LAMBDA(id, [](const CameraPresetDto& d) -> std::optional<std::string> {
    if (d.action != "save" && d.id.empty())
      return "id is required to reach or delete a preset";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(name, [](const CameraPresetDto& d) -> std::optional<std::string> {
    if (d.action == "save" && d.name.empty())
      return "name is required to save a preset";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
