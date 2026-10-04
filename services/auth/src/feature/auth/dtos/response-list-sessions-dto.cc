#include "response-list-sessions-dto.hxx"

Json::Value ResponseListSessionsDto::toJson() const
{
  Json::Value list(Json::arrayValue);
  for (const auto& session : sessions) {
    Json::Value item(Json::objectValue);
    item["id"] = session.id;
    item["platform"] = sessionPlatformToString(session.platform);
    item["deviceName"] = session.deviceName.empty()
                             ? Json::Value(Json::nullValue)
                             : Json::Value(session.deviceName);
    item["createdAt"] = static_cast<Json::Int64>(session.createdAt);
    item["lastSeenAt"] = static_cast<Json::Int64>(session.lastSeenAt);
    item["expiresAt"] = static_cast<Json::Int64>(session.expiresAt);
    item["current"] = session.current;
    list.append(std::move(item));
  }
  Json::Value json(Json::objectValue);
  json["sessions"] = std::move(list);
  return json;
}
