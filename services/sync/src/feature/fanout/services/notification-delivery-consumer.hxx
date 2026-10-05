#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/fanout/repositories/delivery-inbox/delivery-inbox-repository.hxx>
#include <feature/fanout/services/durable-delivery.hxx>
#include <feature/fanout/services/durable-disposition.hxx>
#include <nats/nats-subject.hxx>
#include <notification/notification-delivery-sink.hxx>

#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <optional>
#include <string>

class NatsBus;

class NotificationDeliveryConsumer
{
public:
  struct Dependencies
  {
    NatsBus* bus{nullptr};
    std::function<void(const NotificationDeliveryEvent&)> dispatch;
  };

  struct Config
  {
    std::string stream{nats_subject::kNotificationDeliveryStream};
    std::string durable{"argus-sync-delivery"};
    std::string subject{nats_subject::kNotificationDelivery};
    int maxDeliver{10};
    int poisonMaxAttempts{3};
  };

  NotificationDeliveryConsumer(Dependencies dependencies, Config config);
  ~NotificationDeliveryConsumer();

  void start();
  void stop();
  void requestStop();
  [[nodiscard]] bool drained() const;

  drogon::Task<DurableDisposition> handlePayload(const std::string& payload);

  drogon::Task<DurableDisposition> handle(
      const NotificationDeliveryEvent& event);

private:
  bool trySubscribe();
  void scheduleSubscribeRetry();

  Dependencies dependencies_;
  Config config_;
  DeliveryInboxRepository repository_;
  std::optional<uint64_t> subscription_;
  std::optional<uint64_t> retryTimer_;
  durable_delivery::PendingCount pending_{
      std::make_shared<std::atomic<int64_t>>(0)};
};
