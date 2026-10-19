#include <feature/monitor/nats-health-event-sink.hxx>

#include <json/value.h>
#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>

#include <utility>

NatsHealthEventSink::NatsHealthEventSink(std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus))
{
}

bool NatsHealthEventSink::publish(const CameraHealthEvent& event)
{
  if (!bus_)
    return false;

  Json::Value json(Json::objectValue);
  json["cameraId"] = static_cast<Json::Int64>(event.cameraId);
  json["cameraName"] = event.cameraName;
  json["status"] = std::string(health_monitor::statusName(event.status));
  Json::Value metrics(Json::objectValue);
  metrics["brightness"] = event.metrics.brightness;
  metrics["blur"] = event.metrics.blur;
  metrics["sceneDiff"] = event.metrics.sceneDiff;
  metrics["saturated"] = event.metrics.saturated;
  metrics["edgeRatio"] = event.metrics.edgeRatio;
  metrics["sharpness"] = event.metrics.sharpness;
  metrics["blocksLost"] = event.metrics.blocksLost;
  metrics["shiftPx"] = event.metrics.shiftPx;
  metrics["entropy"] = event.metrics.entropy;
  metrics["infrared"] = event.metrics.infrared;
  json["metrics"] = metrics;
  json["sudden"] = event.metrics.sudden;
  json["link"] = event.link;
  json["onsetAt"] = static_cast<Json::Int64>(event.onsetAtMs);
  json["detectedAt"] = static_cast<Json::Int64>(event.detectedAtMs);

  return bus_->publish(nats_subject::kCameraHealth, json_util::toString(json));
}

bool NatsHealthEventSink::publishPrivacy(const CameraPrivacyEvent& event)
{
  if (!bus_)
    return false;

  Json::Value json(Json::objectValue);
  json["cameraId"] = Json::Int64(event.cameraId);
  json["cameraName"] = event.cameraName;
  json["privacy"] = cameraPrivacyStateToString(event.state);
  json["obeyed"] = event.obeyed;
  json["detectedAt"] = Json::Int64(event.detectedAtMs);

  return bus_->publish(nats_subject::kCameraPrivacy, json_util::toString(json));
}
