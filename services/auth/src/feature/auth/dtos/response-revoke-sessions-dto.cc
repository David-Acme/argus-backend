#include "response-revoke-sessions-dto.hxx"

Json::Value ResponseRevokeSessionsDto::toJson() const
{
  Json::Value ids(Json::arrayValue);
  for (const auto& id : revoked)
    ids.append(id);
  Json::Value json(Json::objectValue);
  json["revoked"] = std::move(ids);
  json["current"] = current;
  return json;
}
