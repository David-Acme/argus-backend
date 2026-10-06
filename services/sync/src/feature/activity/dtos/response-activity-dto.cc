#include "response-activity-dto.hxx"

Json::Value ResponseActivityDto::toJson() const
{
  Json::Value json(Json::objectValue);
  Json::Value items(Json::arrayValue);
  for (const auto& item : page.items)
    items.append(item.toActivityJson());
  json["items"] = std::move(items);
  json["nextCursor"] = page.nextCursor ? Json::Value(*page.nextCursor) : Json::Value();
  return json;
}
