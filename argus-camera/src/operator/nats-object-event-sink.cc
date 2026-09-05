#include <operator/nats-object-event-sink.hxx>

#include <nats/nats.h>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>

NatsObjectEventSink::NatsObjectEventSink(std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus))
{
}

bool NatsObjectEventSink::publish(const ObjectDetectedEvent& event)
{
  if (!bus_)
    return false;
  const std::string payload =
      json_util::toString(object_event::toJson(event));
  return bus_->publish(nats_subject::kCameraObjectDetected, payload);
}

void NatsObjectEventSink::ensureStream(const std::string& natsUrl)
{
  natsConnection* connection = nullptr;
  natsStatus status = natsConnection_ConnectTo(&connection, natsUrl.c_str());
  if (status != NATS_OK) {
    LOG_WARN << "Camera JetStream: connect failed (" << nats_GetLastError(nullptr)
             << "); events publish without retention";
    return;
  }

  jsCtx* js = nullptr;
  status = natsConnection_JetStream(&js, connection, nullptr);
  if (status != NATS_OK) {
    LOG_WARN << "Camera JetStream: context unavailable; events publish"
                " without retention";
    natsConnection_Destroy(connection);
    return;
  }

  static const char* kSubjects[] = {"argus.camera.v1.change",
                                    "argus.camera.v1.object_detected"};
  jsStreamConfig config;
  jsStreamConfig_Init(&config);
  config.Name = "ARGUS_CAMERA";
  config.Subjects = kSubjects;
  config.SubjectsLen = 2;
  config.Retention = js_LimitsPolicy;
  config.MaxAge = 7LL * 24 * 60 * 60 * 1000000000; // 7 days, in nanoseconds
  config.Storage = js_FileStorage;

  jsErrCode errCode = jsErrCode(0);
  status = js_AddStream(nullptr, js, &config, nullptr, &errCode);
  if (status == NATS_OK)
    LOG_INFO << "Camera JetStream: stream ARGUS_CAMERA ready (7d retention)";
  else if (errCode == JSStreamNameExistErr)
    LOG_INFO << "Camera JetStream: stream ARGUS_CAMERA already present";
  else
    LOG_WARN << "Camera JetStream: stream ensure failed (status=" << status
             << " err=" << errCode << "); events publish without retention";

  jsCtx_Destroy(js);
  natsConnection_Destroy(connection);
}
