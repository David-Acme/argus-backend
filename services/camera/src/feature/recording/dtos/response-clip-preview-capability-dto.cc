#include "response-clip-preview-capability-dto.hxx"

Json::Value ResponseClipPreviewCapabilityDto::toJson() const
{
  Json::Value json;
  json["token"] = token;
  json["expiresAt"] = expiresAt;
  return json;
}
