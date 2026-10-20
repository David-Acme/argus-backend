#include <feature/operator/object-event.hxx>
#include <utility>

Json::Value object_event::toJson(const ObjectDetectedEvent& event)
{
  Json::Value json(Json::objectValue);
  json["schemaVersion"] = event.schemaVersion;
  json["eventId"] = event.eventId;
  json["cameraId"] = static_cast<Json::Int64>(event.cameraId);
  json["cameraName"] = event.cameraName;
  json["rule"] = event.rule;
  json["severity"] = eventSeverityToString(event.severity);
  json["escalated"] = event.escalated;
  json["night"] = event.night;
  if (event.knownPersonId)
    json["knownPersonId"] = static_cast<Json::Int64>(*event.knownPersonId);
  json["capturedAt"] = static_cast<Json::Int64>(event.capturedAtMs);
  json["detectedAt"] = static_cast<Json::Int64>(event.detectedAtMs);
  json["publishedAt"] = static_cast<Json::Int64>(event.publishedAtMs);
  if (event.dwellMs > 0)
    json["dwellMs"] = static_cast<Json::Int64>(event.dwellMs);
  if (event.trackId > 0)
    json["trackId"] = static_cast<Json::Int64>(event.trackId);

  Json::Value frame(Json::objectValue);
  frame["width"] = event.frameWidth;
  frame["height"] = event.frameHeight;
  json["frame"] = frame;

  Json::Value objects(Json::arrayValue);
  for (const auto& object : event.objects) {
    Json::Value entry(Json::objectValue);
    entry["class"] = object.name;
    entry["confidence"] = object.confidence;
    Json::Value bbox(Json::objectValue);
    bbox["x"] = object.x;
    bbox["y"] = object.y;
    bbox["w"] = object.w;
    bbox["h"] = object.h;
    entry["bbox"] = bbox;
    if (object.personId > 0) {
      entry["personId"] = static_cast<Json::Int64>(object.personId);
      entry["identity"] = object.identity;
      entry["identityConfidence"] = object.identityConfidence;
    }
    if (object.trackId > 0) {
      entry["trackId"] = static_cast<Json::Int64>(object.trackId);
      entry["firstSeenMs"] = static_cast<Json::Int64>(object.firstSeenMs);
      entry["lastSeenMs"] = static_cast<Json::Int64>(object.lastSeenMs);
      entry["dwellMs"] = static_cast<Json::Int64>(object.dwellMs);
      entry["observationId"] = object.observationId;
      if (!object.identityState.empty()) {
        entry["identityState"] = object.identityState;
        entry["identifyAttempts"] = object.identifyAttempts;
      }
      if (object.scoreSamples > 0) {
        entry["scoreMedian"] = object.scoreMedian;
        entry["scoreSamples"] = object.scoreSamples;
      }
      if (object.trackWindows > 0) {
        entry["zoneWindows"] = object.zoneWindows;
        entry["trackWindows"] = object.trackWindows;
        entry["areaSpread"] = object.areaSpread;
      }
    }
    if (object.zoneKind)
      entry["zoneKind"] = zoneTypeToString(*object.zoneKind);
    if (!object.zoneName.empty())
      entry["zoneName"] = object.zoneName;
    if (!object.signature.empty())
      entry["signature"] = object.signature;
    objects.append(entry);
  }
  json["objects"] = std::move(objects);
  json["doubtful"] = event.doubtful;
  json["lookable"] = event.lookable;
  if (!event.secondLook.empty()) {
    json["secondLook"] = event.secondLook;
    json["lookConfidence"] = event.lookConfidence;
  }
  return json;
}
