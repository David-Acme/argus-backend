#pragma once

#include <cstddef>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <outbox/transactional-outbox.hxx>
#include <string>
#include <string_view>
#include <sync/identity-change-sink.hxx>

class NatsBus;

class NatsIdentityChangeSink : public IdentityChangeSink
{
public:
  struct Config
  {
    int retryMs{outbox::kRetryMs};
    std::string changeSubject;
    std::string actionSubject;
    std::string streamName;
  };

  NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus, const Config& config);
  ~NatsIdentityChangeSink() override = default;
  NatsIdentityChangeSink(const NatsIdentityChangeSink&) = delete;
  NatsIdentityChangeSink& operator=(const NatsIdentityChangeSink&) = delete;
  NatsIdentityChangeSink(NatsIdentityChangeSink&&) = delete;
  NatsIdentityChangeSink& operator=(NatsIdentityChangeSink&&) = delete;

  [[nodiscard]] drogon::Task<void>
  publishCatalog(const IdentityCatalogInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  publishUsersAudit(const UserAuditInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = outbox::kMaxPayloadBytes;
  static constexpr std::string_view kEventIdPrefix = "identity-change:";
  static constexpr std::string_view kActionIdPrefix = "identity-action:";

  [[nodiscard]] static outbox::OutboxRepository repository();

private:
  struct RecordInput
  {
    TableName table;
    int64_t recordId{0};
    std::string payload;
    drogon::orm::DbClient* client{nullptr};
  };

  [[nodiscard]] drogon::Task<void> record(RecordInput input) const;

  const std::string changeSubject_;
  const std::string actionSubject_;
  outbox::TransactionalOutbox outbox_;
};
