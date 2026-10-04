#include "response-rtc-token-dto.hxx"

#include <utility>

Json::Value ResponseRtcTokenDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["url"] = url;
  json["token"] = token;
  json["room"] = room;
  json["identity"] = identity;
  json["agentIdentity"] = agentIdentity;
  json["callId"] = callId;
  json["expiresAt"] = static_cast<Json::Int64>(expiresAt);
  if (call) {
    Json::Value details(Json::objectValue);
    details["kind"] = call->kind;
    details["summary"] = call->summary;
    details["lang"] = call->lang;
    if (call->cameraId > 0)
      details["cameraId"] = static_cast<Json::Int64>(call->cameraId);
    if (!call->cameraName.empty())
      details["cameraName"] = call->cameraName;
    if (call->episodeId > 0)
      details["episodeId"] = static_cast<Json::Int64>(call->episodeId);
    json["call"] = std::move(details);
  }
  return json;
}
