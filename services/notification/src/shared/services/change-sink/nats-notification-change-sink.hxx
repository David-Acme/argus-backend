#pragma once

#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <outbox/transactional-outbox.hxx>
#include <string>
#include <string_view>
#include <sync/user-change-sink.hxx>

class NatsBus;

class NatsNotificationChangeSink : public AuditSink
{
public:
  struct Config
  {
    int retryMs{outbox::kRetryMs};
    std::string publishSubject;
    std::string streamName;
  };

  NatsNotificationChangeSink(std::shared_ptr<NatsBus> bus,
                             const Config& config);
  ~NatsNotificationChangeSink() override = default;
  NatsNotificationChangeSink(const NatsNotificationChangeSink&) = delete;
  NatsNotificationChangeSink& operator=(const NatsNotificationChangeSink&) =
      delete;
  NatsNotificationChangeSink(NatsNotificationChangeSink&&) = delete;
  NatsNotificationChangeSink& operator=(NatsNotificationChangeSink&&) = delete;

  [[nodiscard]] drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = outbox::kMaxPayloadBytes;
  static constexpr std::string_view kEventIdPrefix = "notification-change:";

  [[nodiscard]] static outbox::OutboxRepository repository();

private:
  const std::string subject_;
  outbox::TransactionalOutbox outbox_;
};
