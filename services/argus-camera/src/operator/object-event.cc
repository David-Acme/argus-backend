#include <operator/object-event.hxx>

Json::Value object_event::toJson(const ObjectDetectedEvent& event)
{
  Json::Value json(Json::objectValue);
  json["cameraId"] = Json::Int64(event.cameraId);
  json["cameraName"] = event.cameraName;
  json["rule"] = event.rule;
  json["severity"] = event.severity;
  json["escalated"] = event.escalated;
  if (event.knownPersonId)
    json["knownPersonId"] = Json::Int64(*event.knownPersonId);
  json["detectedAt"] = Json::Int64(event.detectedAtMs);

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
    objects.append(entry);
  }
  json["objects"] = objects;
  return json;
}
