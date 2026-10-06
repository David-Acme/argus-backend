#include "response-module-data-dto.hxx"

#include <utility>

Json::Value ResponseModuleDataDto::toJson() const
{
  Json::Value list(Json::arrayValue);
  for (const auto& view : owners) {
    Json::Value owner(Json::objectValue);
    owner["owner"] = view.owner;
    owner["reachable"] = view.reach != OwnerReach::Unreachable;
    owner["reported"] = view.reach == OwnerReach::Answered;
    Json::Value items(Json::arrayValue);
    for (const auto& item : view.summary.items) {
      Json::Value entry(Json::objectValue);
      entry["kind"] = item.kind;
      entry["count"] = static_cast<Json::Int64>(item.count);
      items.append(entry);
    }
    owner["items"] = items;
    owner["bytes"] = static_cast<Json::Int64>(view.summary.bytes);
    list.append(owner);
  }
  Json::Value json(Json::objectValue);
  json["owners"] = std::move(list);
  return json;
}
