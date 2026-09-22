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

// Productivity-domain change funnel over argus.productivity.v1.change: every
// emit and audit lands in the productivity-owned outbox first, and a worker
// publishes from there, marking a row sent only after the JetStream PubAck.
// Nothing else in the tree captures this subject, so the sink owns the stream
// that carries it.
class NatsProductivityChangeSink : public UserChangeSink
{
public:
  struct Config
  {
    int retryMs{500};
    // JetStream targets; empty keeps the production change subject and stream.
    std::string publishSubject;
    std::string streamName;
  };

  NatsProductivityChangeSink(std::shared_ptr<NatsBus> bus, Config config);
  ~NatsProductivityChangeSink() override;
  NatsProductivityChangeSink(const NatsProductivityChangeSink&) = delete;
  NatsProductivityChangeSink& operator=(const NatsProductivityChangeSink&) =
      delete;

  [[nodiscard]] drogon::Task<void>
  emitUser(int64_t userId, const SocketEmitDto& body) const override;
  [[nodiscard]] drogon::Task<void>
  emitUsers(const std::vector<int64_t>& userIds,
            const SocketEmitDto& body) const override;
  [[nodiscard]] drogon::Task<void>
  publishAudit(const UserAuditInput& input) const override;

  // Starts the publisher; call once after the schema is applied.
  void reconcile();

  // A payload the broker would refuse is not written: one such row would stop
  // every change queued behind it for ever. 256 KiB, past any real frame.
  static constexpr std::size_t kMaxPayloadBytes = std::size_t{256} * 1024;

private:
  [[nodiscard]] drogon::Task<void>
  emit(const std::vector<int64_t>& userIds,
       const SocketEmitDto& body) const;
  [[nodiscard]] drogon::Task<void> enqueue(std::string eventId,
                                           std::string payloadJson) const;
  bool ensureStream() const;
  void flushLoop();
  bool flush(const ChangeOutboxRow& row);

  std::shared_ptr<NatsBus> bus_;
  ChangeOutboxRepository outbox_;
  const Config config_;
  const std::string subject_;
  const std::string stream_;
  // A publish is a JetStream publish: without the stream the broker stores
  // nothing, so the row that proves the feed is moving can never settle. A
  // refused publish clears this, so the ensure runs again instead of latching.
  std::atomic<bool> streamReady_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> workerStarted_{false};
  std::mutex wakeMutex_;
  mutable std::condition_variable wake_;
  std::thread worker_;
};
