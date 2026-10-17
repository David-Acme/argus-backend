#include "response-clips-dto.hxx"

Json::Value ResponseClipsDto::toJson() const
{
  Json::Value json;
  json["cameraId"] = Json::Int64(cameraId);
  Json::Value list(Json::arrayValue);
  for (const auto& clip : clips)
    list.append(clip.toJson());
  json["clips"] = std::move(list);
  return json;
}
