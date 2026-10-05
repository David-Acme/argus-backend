#include "response-visitor-dto.hxx"

#include <string>
#include <utility>

Json::Value ResponseVisitorDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(visitor.id);
  json["name"] = visitor.name;
  json["category"] = std::string(personCategoryToString(visitor.category));
  json["note"] = visitor.note;
  json["visitorNumber"] = visitor.visitorNumber
                              ? Json::Value(static_cast<Json::Int64>(*visitor.visitorNumber))
                              : Json::Value();
  json["visitCount"] = static_cast<Json::Int64>(visitor.visitCount);
  json["firstSeenAt"] = static_cast<Json::Int64>(visitor.firstSeenAt);
  json["lastSeenAt"] = static_cast<Json::Int64>(visitor.lastSeenAt);
  json["sampleCount"] = static_cast<Json::Int64>(visitor.sampleCount);
  json["coverSampleId"] = visitor.coverSampleId
                              ? Json::Value(static_cast<Json::Int64>(*visitor.coverSampleId))
                              : Json::Value();
  Json::Value cameras(Json::arrayValue);
  for (const int64_t camera : visitor.cameraIds)
    cameras.append(static_cast<Json::Int64>(camera));
  json["cameraIds"] = std::move(cameras);
  return json;
}

Json::Value ResponseVisitorListDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["recognitionEnabled"] = recognitionEnabled;
  Json::Value items(Json::arrayValue);
  for (const auto& visitor : visitors)
    items.append(ResponseVisitorDto{.visitor = visitor}.toJson());
  json["visitors"] = std::move(items);
  return json;
}
