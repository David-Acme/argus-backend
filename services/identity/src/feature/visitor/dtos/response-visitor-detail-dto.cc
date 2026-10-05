#include "response-visitor-detail-dto.hxx"

#include <feature/visitor/dtos/response-visitor-dto.hxx>

#include <utility>

Json::Value ResponseVisitorDetailDto::toJson() const
{
  Json::Value json = ResponseVisitorDto{.visitor = visitor}.toJson();
  Json::Value samplesJson(Json::arrayValue);
  for (const auto& sample : samples) {
    Json::Value item(Json::objectValue);
    item["id"] = static_cast<Json::Int64>(sample.id);
    item["quality"] = sample.quality;
    item["cameraId"] = sample.cameraId ? Json::Value(static_cast<Json::Int64>(*sample.cameraId))
                                       : Json::Value();
    item["hasCrop"] = sample.hasCrop;
    item["createdAt"] = static_cast<Json::Int64>(sample.createdAt);
    samplesJson.append(item);
  }
  json["samples"] = std::move(samplesJson);
  Json::Value visitsJson(Json::arrayValue);
  for (const auto& visit : visits) {
    Json::Value item(Json::objectValue);
    item["id"] = static_cast<Json::Int64>(visit.id);
    item["cameraId"] = static_cast<Json::Int64>(visit.cameraId);
    item["startedAt"] = static_cast<Json::Int64>(visit.startedAt);
    item["lastSeenAt"] = static_cast<Json::Int64>(visit.lastSeenAt);
    item["sightings"] = static_cast<Json::Int64>(visit.sightings);
    visitsJson.append(item);
  }
  json["visits"] = std::move(visitsJson);
  Json::Value patternJson(Json::objectValue);
  Json::Value weekdays(Json::arrayValue);
  for (const int day : pattern.weekdays)
    weekdays.append(day);
  patternJson["weekdays"] = std::move(weekdays);
  patternJson["usualHour"] =
      pattern.usualHour ? Json::Value(*pattern.usualHour) : Json::Value();
  patternJson["visitsConsidered"] = pattern.visitsConsidered;
  json["pattern"] = std::move(patternJson);
  return json;
}
