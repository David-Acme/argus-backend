#include "response-voiceprint-challenge-dto.hxx"

#include <cmath>

Json::Value ResponseVoiceprintChallengeDto::toJson() const
{
  Json::Value phrases(Json::arrayValue);
  for (const auto& phrase : challenge.phrases)
    phrases.append(phrase);

  Json::Value json(Json::objectValue);
  json["challengeId"] = challenge.challengeId;
  json["phrases"] = phrases;
  json["expiresAt"] = static_cast<Json::Int64>(challenge.expiresAt);
  json["lang"] = voiceLangToString(challenge.lang);
  json["consentVersion"] = consentVersion;
  json["samplesRequired"] = samplesRequired;
  json["minSpeechSeconds"] = std::round(minSpeechSeconds * 100.0) / 100.0;
  return json;
}
