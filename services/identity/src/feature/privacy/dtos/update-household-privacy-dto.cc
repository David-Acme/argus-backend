#include "update-household-privacy-dto.hxx"

#include <string>

namespace
{
std::optional<bool> boolOf(const Json::Value& json, const char* key)
{
  if (json.isMember(key) && json[key].isBool())
    return json[key].asBool();
  return std::nullopt;
}
}

UpdateHouseholdPrivacyDto
UpdateHouseholdPrivacyDto::fromJson(const Json::Value& json)
{
  UpdateHouseholdPrivacyDto dto;
  dto.presence = boolOf(json, "presence");
  dto.faceCameras = boolOf(json, "faceCameras");
  dto.voiceLearning = boolOf(json, "voiceLearning");
  dto.cameraAudio = boolOf(json, "cameraAudio");
  dto.visitorRecognition = boolOf(json, "visitorRecognition");
  dto.acknowledge = boolOf(json, "acknowledge").value_or(false);

  START_VALIDATION(UpdateHouseholdPrivacyDto, dto)
  CUSTOM_LAMBDA(body, [](const UpdateHouseholdPrivacyDto& value)
                         -> std::optional<std::string> {
    if (!value.presence && !value.faceCameras && !value.voiceLearning &&
        !value.cameraAudio && !value.visitorRecognition)
      return "at least one boolean switch is required";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(acknowledge, [](const UpdateHouseholdPrivacyDto& value)
                    -> std::optional<std::string> {
    if (value.visitorRecognition.value_or(false) && !value.acknowledge)
      return "turning on visitorRecognition requires acknowledge=true";
    return std::nullopt;
  })
  END_VALIDATION()
  return dto;
}
