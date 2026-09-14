#include <monitor/nats-health-event-sink.hxx>

#include <json/value.h>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>

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
  json["cameraId"] = Json::Int64(event.cameraId);
  json["cameraName"] = event.cameraName;
  json["status"] = health_monitor::statusName(event.status);
  Json::Value metrics(Json::objectValue);
  metrics["brightness"] = event.metrics.brightness;
  metrics["blur"] = event.metrics.blur;
  metrics["sceneDiff"] = event.metrics.sceneDiff;
  json["metrics"] = metrics;
  json["detectedAt"] = Json::Int64(event.detectedAtMs);

  return bus_->publish(nats_subject::kCameraHealth, json_util::toString(json));
}
