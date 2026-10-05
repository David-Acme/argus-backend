#include "response-visitor-crop-dto.hxx"

Json::Value ResponseVisitorCropCapabilityDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["token"] = token;
  json["expiresAt"] = static_cast<Json::Int64>(expiresAt);
  return json;
}

Json::Value ResponseVisitorCropImageDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["mimeType"] = mimeType;
  json["base64"] = base64;
  return json;
}
