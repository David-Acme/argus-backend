#include "response-voiceprint-status-dto.hxx"

#include <cmath>

Json::Value ResponseVoiceprintStatusDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["available"] = status.available;
  json["enrolled"] = status.enrolled;
  json["stale"] = status.stale;
  json["model"] = status.model;
  json["sampleCount"] = status.sampleCount;
  json["enrolledAt"] =
      status.enrolled ? Json::Value(static_cast<Json::Int64>(status.enrolledAt))
                      : Json::Value();
  json["method"] = status.enrolled
                       ? Json::Value(voiceprintMethodToString(status.method))
                       : Json::Value();
  json["consentVersion"] = status.consentVersion;
  json["samplesRequired"] = status.samplesRequired;
  json["minSpeechSeconds"] =
      std::round(status.minSpeechSeconds * 100.0) / 100.0;
  return json;
}
