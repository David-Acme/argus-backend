#include "response-module-impact-dto.hxx"

#include <feature/modules/dtos/module-json.hxx>
#include <feature/modules/dtos/response-module-data-dto.hxx>

#include <string>
#include <utility>

namespace
{
Json::Value count(const std::optional<std::int64_t>& value)
{
  return value ? Json::Value(static_cast<Json::Int64>(*value)) : Json::Value();
}

Json::Value strings(const std::vector<std::string>& values)
{
  Json::Value list(Json::arrayValue);
  for (const auto& value : values)
    list.append(value);
  return list;
}
}

Json::Value ResponseModuleImpactDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["moduleId"] = impact.moduleId;
  json["action"] = std::string(impactActionToString(impact.action));
  json["allowed"] = !impact.refusal.has_value();
  if (impact.refusal) {
    Json::Value refusal(Json::objectValue);
    refusal["code"] = std::string(impact.refusal->wireCode());
    refusal["message"] = std::string(impact.refusal->message);
    json["refusal"] = std::move(refusal);
  }
  else {
    json["refusal"] = Json::Value();
  }

  Json::Value stops(Json::arrayValue);
  for (const auto& stop : impact.stops) {
    Json::Value entry(Json::objectValue);
    entry["kind"] = stop.kind;
    entry["count"] = count(stop.count);
    stops.append(std::move(entry));
  }
  json["stops"] = std::move(stops);

  Json::Value holders(Json::arrayValue);
  for (const auto& holder : impact.roleHolders) {
    Json::Value entry(Json::objectValue);
    entry["userId"] = static_cast<Json::Int64>(holder.userId);
    entry["name"] = holder.name;
    entry["lastName"] = holder.lastName.empty() ? Json::Value() : Json::Value(holder.lastName);
    entry["role"] = holder.role;
    entry["isActive"] = holder.isActive;
    holders.append(std::move(entry));
  }
  json["roleHolders"] = std::move(holders);
  json["roleEffect"] = impact.roleEffect;
  json["reassignRoles"] = strings(impact.reassignRoles);

  Json::Value invitations(Json::arrayValue);
  for (const auto& invitation : impact.invitations) {
    Json::Value entry(Json::objectValue);
    entry["id"] = static_cast<Json::Int64>(invitation.id);
    entry["role"] = invitation.role;
    entry["createdBy"] = static_cast<Json::Int64>(invitation.createdBy);
    entry["createdByName"] = invitation.createdByName;
    entry["expiresAt"] = static_cast<Json::Int64>(invitation.expiresAt);
    invitations.append(std::move(entry));
  }
  json["invitations"] = std::move(invitations);

  json["data"] = ResponseModuleDataDto{.owners = impact.data}.toJson();
  json["filesBytes"] = static_cast<Json::Int64>(impact.filesBytes);

  Json::Value keeps(Json::arrayValue);
  for (const auto& item : impact.keepsRunning) {
    Json::Value entry(Json::objectValue);
    entry["id"] = item.id;
    Json::Value localized(Json::objectValue);
    localized["es"] = item.text.es;
    localized["en"] = item.text.en;
    entry["text"] = std::move(localized);
    keeps.append(std::move(entry));
  }
  json["keepsRunning"] = std::move(keeps);
  json["unreachable"] = strings(impact.unreachable);
  json["roleMoves"] = module_json::roleMoves(impact.roleMoves);
  return json;
}
