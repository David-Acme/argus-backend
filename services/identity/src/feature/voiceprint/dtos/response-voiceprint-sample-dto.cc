#include "response-voiceprint-sample-dto.hxx"

#include <cmath>

namespace
{
Json::Value problemOf(VoiceprintOutcome outcome)
{
  switch (outcome) {
    case VoiceprintOutcome::Ok:
      return {};
    case VoiceprintOutcome::SampleTooShort:
      return "too_short";
    case VoiceprintOutcome::SampleTooNoisy:
      return "too_noisy";
    case VoiceprintOutcome::SampleClipped:
      return "clipped";
    case VoiceprintOutcome::Unavailable:
      return "unavailable";
    default:
      break;
  }
  return "invalid";
}
}

Json::Value ResponseVoiceprintSampleDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["accepted"] = check.outcome == VoiceprintOutcome::Ok;
  json["problem"] = problemOf(check.outcome);
  json["speechSeconds"] = std::round(check.speechSeconds * 100.0) / 100.0;
  json["snrDb"] = std::round(check.snrDb * 100.0) / 100.0;
  json["minSpeechSeconds"] = std::round(check.minSpeechSeconds * 100.0) / 100.0;
  json["minSnrDb"] = std::round(check.minSnrDb * 100.0) / 100.0;
  return json;
}
