#include "response-presence-dto.hxx"

Json::Value ResponsePresenceDto::toJson() const
{
  Json::Value list(Json::arrayValue);
  for (const PresenceUserView& person : people) {
    Json::Value item(Json::objectValue);
    item["userId"] = static_cast<Json::Int64>(person.userId);
    item["state"] = presenceStateToString(person.overall);
    item["since"] = static_cast<Json::Int64>(person.since);
    Json::Value environments(Json::arrayValue);
    for (const PresenceRow& row : person.environments) {
      Json::Value environment(Json::objectValue);
      environment["environmentId"] = static_cast<Json::Int64>(row.environmentId);
      environment["state"] = presenceStateToString(row.state);
      environment["since"] = static_cast<Json::Int64>(row.since);
      environments.append(std::move(environment));
    }
    item["environments"] = std::move(environments);
    list.append(std::move(item));
  }
  Json::Value json(Json::objectValue);
  json["people"] = std::move(list);
  return json;
}
