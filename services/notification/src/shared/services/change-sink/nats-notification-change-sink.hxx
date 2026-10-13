#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <outbox/transactional-outbox.hxx>
#include <string>
#include <string_view>
#include <sync/user-change-sink.hxx>

class NatsBus;

class NatsNotificationChangeSink : public UserEmitSink
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
  [[nodiscard]] drogon::Task<void>
  emitUsers(const UserEmitInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = outbox::kMaxPayloadBytes;
  static constexpr std::string_view kEventIdPrefix = "notification-change:";

  [[nodiscard]] static outbox::OutboxRepository repository();

private:
  struct RecordInput
  {
    TableName table{TableName::Notification};
    int64_t recordId{0};
    std::string discriminator;
    std::string payload;
    drogon::orm::DbClient* client{nullptr};
  };

  [[nodiscard]] drogon::Task<void> record(RecordInput input) const;

  const std::string subject_;
  mutable std::atomic<uint64_t> transitions_{0};
  outbox::TransactionalOutbox outbox_;
};
