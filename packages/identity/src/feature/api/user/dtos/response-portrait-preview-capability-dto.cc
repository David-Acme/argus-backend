#include "response-portrait-preview-capability-dto.hxx"

Json::Value ResponsePortraitPreviewCapabilityDto::toJson() const
{
  Json::Value json;
  json["token"] = token;
  json["expiresAt"] = Json::Int64(expiresAt);
  return json;
}
