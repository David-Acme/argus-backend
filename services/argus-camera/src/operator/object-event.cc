#include <operator/object-event.hxx>

Json::Value object_event::toJson(const ObjectDetectedEvent& event)
{
  Json::Value json(Json::objectValue);
  json["schemaVersion"] = event.schemaVersion;
  json["eventId"] = event.eventId;
  json["cameraId"] = Json::Int64(event.cameraId);
  json["cameraName"] = event.cameraName;
  json["rule"] = event.rule;
  json["severity"] = event.severity;
  json["escalated"] = event.escalated;
  if (event.knownPersonId)
    json["knownPersonId"] = Json::Int64(*event.knownPersonId);
  json["capturedAt"] = Json::Int64(event.capturedAtMs);
  json["detectedAt"] = Json::Int64(event.detectedAtMs);
  json["publishedAt"] = Json::Int64(event.publishedAtMs);
  if (event.dwellMs > 0)
    json["dwellMs"] = Json::Int64(event.dwellMs);
  if (event.trackId > 0)
    json["trackId"] = Json::Int64(event.trackId);

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
      entry["personId"] = Json::Int64(object.personId);
      entry["identity"] = object.identity;
      entry["identityConfidence"] = object.identityConfidence;
    }
    if (object.trackId > 0) {
      entry["trackId"] = Json::Int64(object.trackId);
      entry["firstSeenMs"] = Json::Int64(object.firstSeenMs);
      entry["lastSeenMs"] = Json::Int64(object.lastSeenMs);
      entry["dwellMs"] = Json::Int64(object.dwellMs);
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
    if (!object.zoneKind.empty())
      entry["zoneKind"] = object.zoneKind;
    if (!object.signature.empty())
      entry["signature"] = object.signature;
    objects.append(entry);
  }
  json["objects"] = objects;
  return json;
}
