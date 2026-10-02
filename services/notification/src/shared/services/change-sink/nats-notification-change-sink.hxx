#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sync/user-change-sink.hxx>

#include <atomic>
#include <cstddef>
#include <runtime/wake-signal.hxx>
#include <sqlite/transaction.hxx>
#include <string>
#include <thread>

class NatsBus;

class NatsNotificationChangeSink : public AuditSink
{
public:
  struct Config
  {
    int retryMs{500};
    std::string publishSubject;
    std::string streamName;
  };

  NatsNotificationChangeSink(std::shared_ptr<NatsBus> bus, Config config);
  ~NatsNotificationChangeSink() override;
  NatsNotificationChangeSink(const NatsNotificationChangeSink&) = delete;
  NatsNotificationChangeSink& operator=(const NatsNotificationChangeSink&) =
      delete;

  [[nodiscard]] drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = std::size_t{256} * 1024;

private:
  [[nodiscard]] drogon::Task<void>
  enqueue(ChangeOutboxEnqueueInput input) const;
  bool ensureStream() const;
  void flushLoop();
  bool flush(const ChangeOutboxRow& row);

  std::shared_ptr<NatsBus> bus_;
  ChangeOutboxRepository outbox_;
  const Config config_;
  const std::string subject_;
  const std::string stream_;
  int64_t nextPurgeMs_{0};
  std::atomic<bool> streamReady_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> workerStarted_{false};
  std::atomic<bool> exited_{false};
  mutable WakeSignal wake_;
  db_transaction::CommitObserver commitObserver_{[this] { wake_.notify(); }};
  std::thread worker_;
};
