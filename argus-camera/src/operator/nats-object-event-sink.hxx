#pragma once

#include <operator/object-event-sink.hxx>

#include <memory>
#include <string>

class NatsBus;

// Publishes object_detected events over NATS. Ensures (best-effort, once)
// the JetStream stream that retains the subject server-side for later
// inspection only — the gateway's subscription is an ephemeral core-NATS
// consumer, so events published while it is down are not replayed on
// restart.
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
