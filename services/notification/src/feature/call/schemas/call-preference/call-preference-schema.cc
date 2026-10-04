#include "call-preference-schema.hxx"

#include <text/json-util.hxx>

#include <algorithm>

namespace
{
CallMode modeOr(const std::string& value, CallMode fallback)
{
  return callModeFromString(value).value_or(fallback);
}

std::vector<int64_t> environmentIds(const std::string& text)
{
  std::vector<int64_t> ids;
  const Json::Value parsed = json_util::fromString(text);
  if (!parsed.isArray())
    return ids;
  for (const auto& entry : parsed) {
    if (entry.isIntegral() && entry.asInt64() > 0)
      ids.push_back(entry.asInt64());
  }
  return ids;
}
}

CallMode CallPreferenceSchema::modeFor(CallTrigger trigger) const
{
  switch (trigger) {
    case CallTrigger::GuardCritical:
      return guardCritical;
    case CallTrigger::GuardIntruder:
      return guardIntruder;
    case CallTrigger::GuardEscalation:
      return guardEscalation;
    case CallTrigger::GuardArrival:
      return guardArrival;
    case CallTrigger::Agenda:
      return agenda;
    case CallTrigger::Assistant:
      return assistant;
  }
  return CallMode::Notify;
}

bool CallPreferenceSchema::mutes(int64_t environmentId) const
{
  return environmentId > 0 &&
         std::ranges::find(mutedEnvironmentIds, environmentId) !=
             mutedEnvironmentIds.end();
}

Json::Value CallPreferenceSchema::toJson() const
{
  Json::Value json(Json::objectValue);
  json["userId"] = static_cast<Json::Int64>(userId);
  json["enabled"] = enabled;
  json["guardCritical"] = callModeToString(guardCritical);
  json["guardIntruder"] = callModeToString(guardIntruder);
  json["guardEscalation"] = callModeToString(guardEscalation);
  json["guardArrival"] = callModeToString(guardArrival);
  json["agenda"] = callModeToString(agenda);
  json["assistant"] = callModeToString(assistant);
  json["quietStartHour"] = quietStartHour;
  json["quietEndHour"] = quietEndHour;
  json["dndUntil"] = static_cast<Json::Int64>(dndUntil);
  json["criticalBypass"] = criticalBypass;
  Json::Value muted(Json::arrayValue);
  for (const int64_t id : mutedEnvironmentIds)
    muted.append(static_cast<Json::Int64>(id));
  json["mutedEnvironmentIds"] = std::move(muted);
  json["updatedAt"] = static_cast<Json::Int64>(updatedAt);
  return json;
}

CallPreferenceSchema CallPreferenceSchema::defaultsFor(int64_t userId)
{
  CallPreferenceSchema preference;
  preference.userId = userId;
  return preference;
}

CallPreferenceSchema CallPreferenceSchema::fromRow(const drogon::orm::Row& row)
{
  CallPreferenceSchema preference;
  preference.userId = row["user_id"].as<int64_t>();
  preference.enabled = row["enabled"].as<int>() != 0;
  preference.guardCritical =
      modeOr(row["guard_critical"].as<std::string>(), CallMode::Call);
  preference.guardIntruder =
      modeOr(row["guard_intruder"].as<std::string>(), CallMode::Call);
  preference.guardEscalation =
      modeOr(row["guard_escalation"].as<std::string>(), CallMode::Call);
  preference.guardArrival =
      modeOr(row["guard_arrival"].as<std::string>(), CallMode::Off);
  preference.agenda = modeOr(row["agenda"].as<std::string>(), CallMode::Call);
  preference.assistant =
      modeOr(row["assistant"].as<std::string>(), CallMode::Call);
  preference.quietStartHour = row["quiet_start_hour"].as<int>();
  preference.quietEndHour = row["quiet_end_hour"].as<int>();
  preference.dndUntil = row["dnd_until"].as<int64_t>();
  preference.criticalBypass = row["critical_bypass"].as<int>() != 0;
  preference.mutedEnvironmentIds =
      environmentIds(row["muted_environments"].as<std::string>());
  preference.updatedAt = row["updated_at"].as<int64_t>();
  return preference;
}
