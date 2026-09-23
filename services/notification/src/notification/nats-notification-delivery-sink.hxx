#pragma once

#include <memory>
#include <notification/notification-delivery-sink.hxx>
#include <string>

class NatsBus;

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

  bool ensureStream() const override;

  bool publish(const NotificationDeliveryEvent& event) const override;

private:
  std::shared_ptr<NatsBus> bus_;
  Config config_;
};
