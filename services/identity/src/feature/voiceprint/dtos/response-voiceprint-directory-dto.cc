#include "response-voiceprint-directory-dto.hxx"

Json::Value ResponseVoiceprintDirectoryDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["available"] = directory.available;
  Json::Value recognized(Json::arrayValue);
  for (const auto& entry : directory.recognized) {
    Json::Value item(Json::objectValue);
    item["userId"] = static_cast<Json::Int64>(entry.userId);
    item["since"] = static_cast<Json::Int64>(entry.since);
    item["updatedAt"] = static_cast<Json::Int64>(entry.updatedAt);
    recognized.append(std::move(item));
  }
  json["recognized"] = std::move(recognized);
  return json;
}
