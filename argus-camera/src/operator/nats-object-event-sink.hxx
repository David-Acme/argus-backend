#pragma once

#include <operator/object-event-sink.hxx>

#include <memory>
#include <string>

class NatsBus;

// Publishes object_detected events over NATS. Ensures (best-effort, once)
// the JetStream stream that retains the subject so offline gateway restarts
// keep the notification budget honest.
class NatsObjectEventSink final : public IObjectEventSink
{
public:
  explicit NatsObjectEventSink(std::shared_ptr<NatsBus> bus);

  bool publish(const ObjectDetectedEvent& event) override;

  // Creates the ARGUS_CAMERA JetStream stream when missing; a failure is
  // logged and core publishing continues (events still flow, retention off).
  static void ensureStream(const std::string& natsUrl);

private:
  std::shared_ptr<NatsBus> bus_;
};