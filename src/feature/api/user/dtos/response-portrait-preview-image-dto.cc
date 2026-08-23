#include "response-portrait-preview-image-dto.hxx"

Json::Value ResponsePortraitPreviewImageDto::toJson() const
{
  Json::Value json;
  json["mimeType"] = mimeType;
  json["base64"] = base64;
  return json;
}
