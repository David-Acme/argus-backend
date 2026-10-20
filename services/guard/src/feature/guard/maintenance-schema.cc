#include "maintenance-schema.hxx"

#include <text/json-util.hxx>

#include <algorithm>
#include <utility>

bool MaintenanceWindow::active(int64_t now) const
{
  return endedAt == 0 && now < endsAt;
}

bool MaintenanceWindow::covers(int64_t cameraId) const
{
  return std::ranges::find(cameraIds, cameraId) != cameraIds.end();
}

Json::Value MaintenanceWindow::toJson(const MaintenanceJsonInput& input) const
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(id);
  json["environmentId"] = static_cast<Json::Int64>(environmentId);
  Json::Value cameras(Json::arrayValue);
  for (const int64_t cameraId : cameraIds)
    cameras.append(static_cast<Json::Int64>(cameraId));
  json["cameraIds"] = cameras;
  json["declaredBy"] = static_cast<Json::Int64>(declaredBy);
  json["declaredByName"] = declaredName;
  json["startedAt"] = static_cast<Json::Int64>(startedAt);
  json["endsAt"] = static_cast<Json::Int64>(endsAt);
  json["endedAt"] = endedAt > 0 ? Json::Value(static_cast<Json::Int64>(endedAt)) : Json::Value();
  json["endReason"] = endReason == MaintenanceEndReason::None
                          ? Json::Value()
                          : Json::Value(maintenanceEndReasonToString(endReason));
  json["active"] = active(input.now);
  json["duress"] = input.owner && duress;
  json["source"] = maintenanceSourceToString(source);
  Json::Value checks(Json::arrayValue);
  for (const ViewCheck& check : viewChecks) {
    Json::Value entry(Json::objectValue);
    entry["cameraId"] = static_cast<Json::Int64>(check.cameraId);
    entry["cameraName"] = check.cameraName;
    entry["state"] = viewCheckStateToString(check.state);
    checks.append(entry);
  }
  json["viewChecks"] = std::move(checks);
  return json;
}

namespace maintenance_schema
{
std::string endedEventId(int64_t windowId)
{
  return "guard-maintenance-ended:" + std::to_string(windowId);
}

Json::Value endedEventJson(const MaintenanceEndedEvent& event)
{
  const MaintenanceWindow& window = event.window;
  Json::Value json(Json::objectValue);
  json["eventId"] = endedEventId(window.id);
  json["maintenanceId"] = static_cast<Json::Int64>(window.id);
  json["environmentId"] = static_cast<Json::Int64>(window.environmentId);
  json["environment"] = event.environmentName;
  Json::Value ids(Json::arrayValue);
  for (const int64_t cameraId : window.cameraIds)
    ids.append(static_cast<Json::Int64>(cameraId));
  json["cameraIds"] = std::move(ids);
  Json::Value names(Json::arrayValue);
  for (const std::string& name : event.cameraNames)
    names.append(name);
  json["cameras"] = std::move(names);
  json["declaredBy"] = static_cast<Json::Int64>(window.declaredBy);
  json["declaredName"] = window.declaredName;
  json["declaredRole"] = userRoleToString(window.declaredRole);
  json["source"] = maintenanceSourceToString(window.source);
  json["startedAt"] = static_cast<Json::Int64>(window.startedAt);
  json["endedAt"] = static_cast<Json::Int64>(window.endedAt);
  json["endReason"] = maintenanceEndReasonToString(window.endReason);
  return json;
}

std::string idsToJson(const std::vector<int64_t>& ids)
{
  Json::Value array(Json::arrayValue);
  for (const int64_t id : ids)
    array.append(static_cast<Json::Int64>(id));
  return json_util::toString(array);
}

std::vector<int64_t> idsFromJson(const std::string& text)
{
  std::vector<int64_t> ids;
  const Json::Value array = json_util::fromString(text);
  if (!array.isArray())
    return ids;
  ids.reserve(array.size());
  for (const auto& entry : array)
    if (entry.isIntegral() && entry.asInt64() > 0)
      ids.push_back(entry.asInt64());
  return ids;
}

std::string viewChecksToJson(const std::vector<ViewCheck>& checks)
{
  Json::Value array(Json::arrayValue);
  for (const ViewCheck& check : checks) {
    Json::Value entry(Json::objectValue);
    entry["cameraId"] = static_cast<Json::Int64>(check.cameraId);
    entry["cameraName"] = check.cameraName;
    entry["state"] = viewCheckStateToString(check.state);
    array.append(entry);
  }
  return json_util::toString(array);
}

std::vector<ViewCheck> viewChecksFromJson(const std::string& text)
{
  std::vector<ViewCheck> checks;
  const Json::Value array = json_util::fromString(text);
  if (!array.isArray())
    return checks;
  for (const auto& entry : array) {
    if (!entry.isObject())
      continue;
    checks.push_back({.cameraId = entry.get("cameraId", 0).asInt64(),
                      .cameraName = entry.get("cameraName", "").asString(),
                      .state = viewCheckStateFromString(entry.get("state", "").asString())
                                   .value_or(ViewCheckState::Pending)});
  }
  return checks;
}
}
