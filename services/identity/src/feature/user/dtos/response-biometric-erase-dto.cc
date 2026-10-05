#include "response-biometric-erase-dto.hxx"

Json::Value ResponseBiometricEraseDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["faces"] = static_cast<Json::UInt64>(faces);
  json["portraits"] = static_cast<Json::UInt64>(portraits);
  json["voiceProfile"] = voiceProfile;
  json["voiceSamples"] = static_cast<Json::UInt64>(voiceSamples);
  return json;
}
