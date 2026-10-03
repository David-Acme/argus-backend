#include "create-voiceprint-challenge-dto.hxx"

CreateVoiceprintChallengeDto
CreateVoiceprintChallengeDto::fromJson(const Json::Value& json)
{
  CreateVoiceprintChallengeDto dto;
  if (json.isMember("lang") && json["lang"].isString())
    dto.lang = json["lang"].asString();

  START_VALIDATION(CreateVoiceprintChallengeDto, dto)
  IS_IN_OPTIONAL(lang, "es", "en")
  END_VALIDATION()
  return dto;
}

std::optional<VoiceLang> CreateVoiceprintChallengeDto::language() const
{
  if (!lang)
    return std::nullopt;
  return voiceLangFromString(*lang);
}
