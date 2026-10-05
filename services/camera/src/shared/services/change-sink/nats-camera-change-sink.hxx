#pragma once

#include <cstddef>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <outbox/transactional-outbox.hxx>
#include <string>
#include <string_view>
#include <sync/camera-change-sink.hxx>

class NatsBus;

class NatsCameraChangeSink : public CameraChangeSink
{
public:
  struct Config
  {
    int retryMs{outbox::kRetryMs};
    std::string publishSubject;
    std::string streamName;
  };

  NatsCameraChangeSink(const std::shared_ptr<NatsBus>& bus,
                       const Config& config);
  ~NatsCameraChangeSink() override = default;
  NatsCameraChangeSink(const NatsCameraChangeSink&) = delete;
  NatsCameraChangeSink& operator=(const NatsCameraChangeSink&) = delete;
  NatsCameraChangeSink(NatsCameraChangeSink&&) = delete;
  NatsCameraChangeSink& operator=(NatsCameraChangeSink&&) = delete;

  [[nodiscard]] drogon::Task<void>
  emitModule(const ModuleEmitInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  publishAudit(const ModuleAuditInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = outbox::kMaxPayloadBytes;
  static constexpr std::string_view kEventIdPrefix = "camera-change:";

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

  const std::string subject_;
  outbox::TransactionalOutbox outbox_;
};
