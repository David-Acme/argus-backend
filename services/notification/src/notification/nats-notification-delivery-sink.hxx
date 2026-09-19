#pragma once

#include <memory>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <string>

class NatsBus;

// Durable delivery publisher over argus.notification.v1.delivery: each
// pending intent is stored by JetStream under its deterministic message id,
// so only a PubAck settles the intent.
class NatsNotificationDeliverySink : public NotificationDeliverySink
{
public:
  struct Config
  {
    std::string stream;
    std::string subject;
  };

  explicit NatsNotificationDeliverySink(std::shared_ptr<NatsBus> bus,
                                        Config config);

  // Creates the notification-owned stream when missing; safe to retry.
  bool ensureStream() const override;

  bool publish(const NotificationDeliveryEvent& event) const override;

private:
  std::shared_ptr<NatsBus> bus_;
  Config config_;
};
