#include "update-privacy-dto.hxx"

#include <string>

namespace
{
std::optional<bool> boolOf(const Json::Value& json, const char* key)
{
  if (json.isMember(key) && json[key].isBool())
    return json[key].asBool();
  return std::nullopt;
}

std::optional<std::string> required(const std::optional<bool>& value,
                                    const char* message)
{
  if (!value)
    return std::string{message};
  return std::nullopt;
}
}

UpdatePrivacyDto UpdatePrivacyDto::fromJson(const Json::Value& json)
{
  UpdatePrivacyDto dto;
  if (json.isMember("noticeVersion") && json["noticeVersion"].isIntegral())
    dto.noticeVersion = json["noticeVersion"].asInt64();
  dto.presence = boolOf(json, "presence");
  dto.faceCameras = boolOf(json, "faceCameras");
  dto.voiceLearning = boolOf(json, "voiceLearning");
  dto.cameraAudio = boolOf(json, "cameraAudio");

  START_VALIDATION(UpdatePrivacyDto, dto)
  IS_POSITIVE(noticeVersion)
  CUSTOM_LAMBDA(presence, [](const UpdatePrivacyDto& value) {
    return required(value.presence, "presence must be a boolean");
  })
  CUSTOM_LAMBDA(faceCameras, [](const UpdatePrivacyDto& value) {
    return required(value.faceCameras, "faceCameras must be a boolean");
  })
  CUSTOM_LAMBDA(voiceLearning, [](const UpdatePrivacyDto& value) {
    return required(value.voiceLearning, "voiceLearning must be a boolean");
  })
  CUSTOM_LAMBDA(cameraAudio, [](const UpdatePrivacyDto& value) {
    return required(value.cameraAudio, "cameraAudio must be a boolean");
  })
  END_VALIDATION()
  return dto;
}
