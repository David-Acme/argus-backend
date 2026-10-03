#include "response-voiceprint-verify-dto.hxx"

Json::Value ResponseVoiceprintVerifyDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["matched"] = result.matched;
  json["score"] = result.score;
  json["threshold"] = result.threshold;
  return json;
}
