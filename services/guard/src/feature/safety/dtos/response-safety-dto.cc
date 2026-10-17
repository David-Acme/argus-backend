#include "response-safety-dto.hxx"

Json::Value ResponseSafetyStatusDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["duressEnabled"] = duressEnabled;
  json["hasPin"] = hasPin;
  json["hasPhrase"] = hasPhrase;
  json["alertsReach"] = safetyReachToString(alertsReach);
  json["appSirenEnabled"] = appSirenEnabled;
  Json::Value cameras(Json::arrayValue);
  for (const auto& camera : maskedCameras) {
    Json::Value entry(Json::objectValue);
    entry["cameraId"] = static_cast<Json::Int64>(camera.cameraId);
    entry["name"] = camera.name;
    cameras.append(std::move(entry));
  }
  json["maskedCameras"] = std::move(cameras);
  return json;
}

Json::Value ResponsePanicDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["alertId"] = static_cast<Json::Int64>(alertId);
  json["sent"] = sent;
  json["repeated"] = repeated;
  return json;
}
