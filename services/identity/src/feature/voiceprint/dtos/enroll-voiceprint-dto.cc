#include "enroll-voiceprint-dto.hxx"

#include <cstddef>
#include <drogon/utils/Utilities.h>

namespace
{
constexpr size_t kMaxFaceEncodedBytes = size_t{14} * 1024 * 1024;
}

EnrollVoiceprintDto EnrollVoiceprintDto::fromJson(const Json::Value& json)
{
  EnrollVoiceprintDto dto;
  if (json.isMember("consent") && json["consent"].isBool())
    dto.consent = json["consent"].asBool();
  if (json.isMember("consentVersion") && json["consentVersion"].isString())
    dto.consentVersion = json["consentVersion"].asString();
  if (json.isMember("challengeId") && json["challengeId"].isString())
    dto.challengeId = json["challengeId"].asString();
  if (json.isMember("face") && json["face"].isString())
    dto.face = json["face"].asString();

  START_VALIDATION(EnrollVoiceprintDto, dto)
  CUSTOM_LAMBDA(consent,
                [](const EnrollVoiceprintDto& value)
                    -> std::optional<std::string> {
                  if (!value.consent)
                    return "explicit consent is required";
                  return std::nullopt;
                })
  IS_NOT_EMPTY(consentVersion)
  MAX_LENGTH(consentVersion, 64)
  IS_NOT_EMPTY(challengeId)
  MAX_LENGTH(challengeId, 128)
  CUSTOM_LAMBDA(face,
                [](const EnrollVoiceprintDto& value)
                    -> std::optional<std::string> {
                  if (value.face.size() > kMaxFaceEncodedBytes)
                    return "face must not exceed 14MB of base64";
                  return std::nullopt;
                })
  END_VALIDATION()
  return dto;
}

std::string EnrollVoiceprintDto::faceImage() const
{
  return face.empty() ? std::string() : drogon::utils::base64Decode(face);
}
