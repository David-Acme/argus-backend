#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/enums.hxx>
#include <shared/repositories/delivery-inbox/delivery-inbox-repository.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class NatsBus;

enum class DeliveryDisposition : uint8_t
{
  Ack = 0,
  Nak,
  Term
};

// Durable consumer over the notification-owned delivery stream. stop() is
// idempotent and runs from the destructor; destroy only while the loop that
// started it is still running.
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
    std::string durable{"argus-gateway-delivery"};
    std::string subject{nats_subject::kNotificationDelivery};
    int maxDeliver{10};
    int poisonMaxAttempts{3};
  };

  NotificationDeliveryConsumer(Dependencies dependencies, Config config);
  ~NotificationDeliveryConsumer();

  void start();
  void stop();

  // Parses, validates and handles one raw payload for the broker settlement.
  drogon::Task<DeliveryDisposition> handlePayload(const std::string& payload);

  // Receipts first, dispatches when new: Ack settles, Nak redelivers, Term
  // drops poison without redelivery.
  drogon::Task<DeliveryDisposition> handle(
      const NotificationDeliveryEvent& event);

private:
  bool trySubscribe();
  void scheduleSubscribeRetry();

  Dependencies dependencies_;
  Config config_;
  DeliveryInboxRepository repository_;
  std::optional<uint64_t> subscription_;
  std::optional<uint64_t> retryTimer_;
};
