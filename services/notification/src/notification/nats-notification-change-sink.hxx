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

class NatsBus;

// Notification-domain change funnel over argus.notification.v1.change: every
// audit lands in the notification-owned outbox first, and a worker publishes
// from there, marking a row sent only after the JetStream PubAck. The subject
// is retained on the change stream this sink owns, kept beside the delivery
// stream rather than inside it: a stream carries one subject set, and the two
// legs are configured apart.
class NatsNotificationChangeSink : public AuditSink
{
public:
  struct Config
  {
    int retryMs{500};
    // JetStream targets; empty keeps the production change subject and stream.
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

  // Starts the publisher; call once after the schema is applied.
  void reconcile();

  // A payload the broker would refuse is not written: one such row would stop
  // every change queued behind it for ever. 256 KiB, past any real frame.
  static constexpr std::size_t kMaxPayloadBytes = std::size_t{256} * 1024;

private:
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
