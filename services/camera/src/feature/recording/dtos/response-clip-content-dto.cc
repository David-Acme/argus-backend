#include "response-clip-content-dto.hxx"

Json::Value ResponseClipContentDto::toJson() const
{
  Json::Value json;
  json["mimeType"] = mimeType;
  json["base64"] = base64;
  json["durationMs"] = durationMs;
  return json;
}
