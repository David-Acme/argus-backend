#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sync/user-change-sink.hxx>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NatsBus;

class NatsProductivityChangeSink : public UserChangeSink
{
public:
  struct Config
  {
    int retryMs{500};
    std::string publishSubject;
    std::string streamName;
  };

  NatsProductivityChangeSink(std::shared_ptr<NatsBus> bus, Config config);
  ~NatsProductivityChangeSink() override;
  NatsProductivityChangeSink(const NatsProductivityChangeSink&) = delete;
  NatsProductivityChangeSink& operator=(const NatsProductivityChangeSink&) =
      delete;

  [[nodiscard]] drogon::Task<void>
  emitUsers(const UserEmitInput& input) const override;
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
  std::atomic<bool> streamReady_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> workerStarted_{false};
  std::atomic<bool> exited_{false};
  std::mutex wakeMutex_;
  mutable std::condition_variable wake_;
  std::thread worker_;
};
