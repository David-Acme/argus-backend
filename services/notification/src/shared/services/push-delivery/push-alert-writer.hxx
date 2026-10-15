#pragma once

#include <shared/repositories/notification/notification-query.hxx>
#include <shared/services/push-delivery/push-delivery-outbox.hxx>
#include <shared/services/push-delivery/push-device.hxx>
#include <shared/services/push-delivery/push-sealer.hxx>

#include <cstdint>
#include <functional>
#include <memory>

struct PushAlertWriterDependencies
{
  std::shared_ptr<const PushDeviceDirectory> devices;
  std::shared_ptr<const PushDeliveryOutbox> outbox;
  std::function<int64_t()> clock;
};

class PushAlertWriter final : public NotificationAlertWriter
{
public:
  explicit PushAlertWriter(PushAlertWriterDependencies dependencies);

  [[nodiscard]] drogon::Task<void>
  write(NotificationAlertRound round) const override;

private:
  std::shared_ptr<const PushDeviceDirectory> devices_;
  std::shared_ptr<const PushDeliveryOutbox> outbox_;
  std::function<int64_t()> clock_;
  PushSealer sealer_;
};
