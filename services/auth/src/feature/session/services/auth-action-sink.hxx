#pragma once

#include <auth/auth-errors.hxx>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/session/repositories/change-outbox/change-outbox-repository.hxx>
#include <sync/auth-change-sink.hxx>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <runtime/wake-signal.hxx>
#include <sqlite/transaction.hxx>

class NatsBus;

class AuthActionSink : public AuthChangeSink
{
public:
  struct Config
  {
    int retryMs{500};
    std::string actionSubject;
    std::string streamName;
    std::string sessionSubject;
    std::string sessionStreamName;
  };

  AuthActionSink(std::shared_ptr<NatsBus> bus, Config config);
  ~AuthActionSink() override;
  AuthActionSink(const AuthActionSink&) = delete;
  AuthActionSink& operator=(const AuthActionSink&) = delete;

  [[nodiscard]] drogon::Task<void>
  publishAction(const AuthActionPublishInput& input) const override;

  [[nodiscard]] drogon::Task<void>
  publishSessionChange(const AuthSessionChangeInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = std::size_t{256} * 1024;

private:
  struct EnqueueInput
  {
    std::string payloadJson;
    std::string eventId;
    std::string subject;
    drogon::orm::DbClient* client{nullptr};
  };

  [[nodiscard]] drogon::Task<void> enqueue(EnqueueInput input) const;
  [[nodiscard]] bool ensureStream() const;
  void flushLoop();
  bool flush(const ChangeOutboxRow& row);

  std::shared_ptr<NatsBus> bus_;
  ChangeOutboxRepository outbox_;
  const Config config_;
  const std::string actionSubject_;
  const std::string stream_;
  const std::string sessionSubject_;
  const std::string sessionStream_;
  int64_t nextPurgeMs_{0};
  std::atomic<bool> streamReady_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> workerStarted_{false};
  std::atomic<bool> exited_{false};
  mutable WakeSignal wake_;
  db_transaction::CommitObserver commitObserver_{[this] { wake_.notify(); }};
  std::thread worker_;
};
