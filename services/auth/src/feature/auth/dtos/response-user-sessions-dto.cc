#include "response-user-sessions-dto.hxx"

Json::Value ResponseUserSessionsDto::toJson() const
{
  Json::Value list(Json::arrayValue);
  for (const auto& user : users) {
    Json::Value sessions(Json::arrayValue);
    for (const auto& session : user.sessions)
      sessions.append(session.toJson());
    Json::Value item(Json::objectValue);
    item["userId"] = static_cast<Json::Int64>(user.userId);
    item["sessions"] = std::move(sessions);
    list.append(std::move(item));
  }
  Json::Value json(Json::objectValue);
  json["users"] = std::move(list);
  return json;
}
