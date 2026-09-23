#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sync/identity-change-sink.hxx>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NatsBus;

class NatsIdentityChangeSink : public IdentityChangeSink
{
public:
  struct Config
  {
    int retryMs{500};
    std::string changeSubject;
    std::string actionSubject;
    std::string streamName;
  };

  NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus, Config config);
  ~NatsIdentityChangeSink() override;
  NatsIdentityChangeSink(const NatsIdentityChangeSink&) = delete;
  NatsIdentityChangeSink& operator=(const NatsIdentityChangeSink&) = delete;

  [[nodiscard]] drogon::Task<void>
  publishCatalog(const IdentityCatalogInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  emitModule(TableName table, const SocketEmitDto& body) const override;
  [[nodiscard]] drogon::Task<void>
  publishModuleAudit(const ModuleAuditInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  publishUsersAudit(const UserAuditInput& input) const override;
  [[nodiscard]] drogon::Task<void>
  publishAction(const UserActionEvent& event) const override;

  void reconcile();

  static constexpr std::size_t kMaxPayloadBytes = std::size_t{256} * 1024;

private:
  [[nodiscard]] drogon::Task<void> enqueueChange(std::string eventId,
                                                 std::string payloadJson) const;
  [[nodiscard]] drogon::Task<void>
  enqueueAction(std::string payloadJson) const;
  bool ensureStream() const;
  void flushLoop();
  bool flush(const ChangeOutboxRow& row);

  std::shared_ptr<NatsBus> bus_;
  ChangeOutboxRepository outbox_;
  const Config config_;
  const std::string changeSubject_;
  const std::string actionSubject_;
  const std::string stream_;
  std::atomic<bool> streamReady_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> workerStarted_{false};
  std::mutex wakeMutex_;
  mutable std::condition_variable wake_;
  std::thread worker_;
};
