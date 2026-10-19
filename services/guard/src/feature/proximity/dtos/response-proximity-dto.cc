#include "response-proximity-dto.hxx"

namespace
{
Json::Value seenJson(const std::optional<int64_t>& seen)
{
  return seen ? Json::Value(static_cast<Json::Int64>(*seen)) : Json::Value(Json::nullValue);
}

Json::Value receiverJson(const ProximityReceiverView& view)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(view.row.id);
  json["name"] = view.row.name;
  json["room"] = view.row.room;
  json["kind"] = proximityReceiverKindToString(view.row.kind);
  json["host"] = view.row.host;
  json["port"] = static_cast<Json::Int64>(view.row.port);
  json["online"] = view.online;
  json["lastSeenAt"] = seenJson(view.lastSeenAt);
  return json;
}

Json::Value sensorJson(const ProximitySensorView& view)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(view.row.id);
  json["name"] = view.row.name;
  json["room"] = view.row.room;
  json["model"] = proximitySensorModelToString(view.row.model);
  json["source"] = proximitySensorSourceToString(view.row.source);
  json["host"] = view.row.host;
  json["port"] = static_cast<Json::Int64>(view.row.port);
  json["entityId"] = view.row.entityId;
  json["cameraId"] =
      view.row.cameraId ? Json::Value(static_cast<Json::Int64>(*view.row.cameraId)) : Json::Value(Json::nullValue);
  json["panOffsetDeg"] = view.row.panOffsetDeg ? Json::Value(*view.row.panOffsetDeg) : Json::Value(Json::nullValue);
  json["online"] = view.online;
  json["lastSeenAt"] = seenJson(view.lastSeenAt);
  return json;
}
}

Json::Value ResponseProximityMeDto::toJson() const
{
  Json::Value json(Json::objectValue);
  Json::Value ble(Json::objectValue);
  ble["enabled"] = me.enabled;
  ble["seed"] = me.seed ? Json::Value(*me.seed) : Json::Value(Json::nullValue);
  ble["rotateSeconds"] = static_cast<Json::Int64>(me.rotateSeconds);
  ble["receivers"] = static_cast<Json::Int64>(me.receivers);
  json["ble"] = std::move(ble);
  json["located"] = me.located;
  return json;
}

Json::Value ResponseProximityReceiversDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["receivers"] = Json::Value(Json::arrayValue);
  for (const ProximityReceiverView& view : receivers)
    json["receivers"].append(receiverJson(view));
  return json;
}

Json::Value ResponseProximityReceiverDto::toJson() const
{
  return receiverJson(receiver);
}

Json::Value ResponseProximitySensorsDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["sensors"] = Json::Value(Json::arrayValue);
  for (const ProximitySensorView& view : sensors)
    json["sensors"].append(sensorJson(view));
  return json;
}

Json::Value ResponseProximitySensorDto::toJson() const
{
  return sensorJson(sensor);
}

Json::Value ResponseProximityPeopleDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["people"] = Json::Value(Json::arrayValue);
  for (const RoomVerdict& verdict : people) {
    Json::Value person(Json::objectValue);
    person["userId"] = static_cast<Json::Int64>(verdict.userId);
    person["room"] = verdict.room;
    person["confidence"] = verdict.confidence;
    person["since"] = static_cast<Json::Int64>(verdict.sinceMs / 1000);
    person["updatedAt"] = static_cast<Json::Int64>(verdict.updatedMs / 1000);
    person["evidence"] = proximityEvidenceKindToString(verdict.strongest);
    json["people"].append(std::move(person));
  }
  return json;
}
