#include "response-list-sessions-dto.hxx"

Json::Value SessionView::toJson() const
{
  Json::Value item(Json::objectValue);
  item["id"] = id;
  item["platform"] = sessionPlatformToString(platform);
  item["deviceName"] = deviceName.empty() ? Json::Value(Json::nullValue)
                                          : Json::Value(deviceName);
  item["createdAt"] = static_cast<Json::Int64>(createdAt);
  item["lastSeenAt"] = static_cast<Json::Int64>(lastSeenAt);
  item["expiresAt"] = static_cast<Json::Int64>(expiresAt);
  item["current"] = current;
  return item;
}

Json::Value ResponseListSessionsDto::toJson() const
{
  Json::Value list(Json::arrayValue);
  for (const auto& session : sessions)
    list.append(session.toJson());
  Json::Value json(Json::objectValue);
  json["sessions"] = std::move(list);
  return json;
}
