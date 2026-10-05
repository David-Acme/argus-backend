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

class NatsProductivityChangeSink : public UserChangeSink
{
public:
  struct Config
  {
    int retryMs{outbox::kRetryMs};
    std::string publishSubject;
    std::string streamName;
  };

  NatsProductivityChangeSink(std::shared_ptr<NatsBus> bus,
                             const Config& config);
  ~NatsProductivityChangeSink() override = default;
  NatsProductivityChangeSink(const NatsProductivityChangeSink&) = delete;
  NatsProductivityChangeSink& operator=(const NatsProductivityChangeSink&) =
      delete;
  NatsProductivityChangeSink(NatsProductivityChangeSink&&) = delete;
  NatsProductivityChangeSink& operator=(NatsProductivityChangeSink&&) = delete;

  [[nodiscard]] drogon::Task<void>
  emitUsers(const UserEmitInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = outbox::kMaxPayloadBytes;
  static constexpr std::string_view kEventIdPrefix = "productivity-change:";

  [[nodiscard]] static outbox::OutboxRepository repository();

private:
  struct RecordInput
  {
    TableName table;
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
