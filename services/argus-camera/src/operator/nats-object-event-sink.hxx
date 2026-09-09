#pragma once

#include <operator/object-event-sink.hxx>

#include <memory>
#include <string>

class NatsBus;

// Publishes object_detected over NATS (JetStream retention is inspection-only).
class NatsObjectEventSink final : public IObjectEventSink
{
public:
  explicit NatsObjectEventSink(std::shared_ptr<NatsBus> bus);

  bool publish(const ObjectDetectedEvent& event) override;

  // Creates the ARGUS_CAMERA JetStream stream when missing.
  static void ensureStream(const std::string& natsUrl);

private:
  std::shared_ptr<NatsBus> bus_;
};
