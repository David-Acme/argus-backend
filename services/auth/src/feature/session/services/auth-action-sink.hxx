#pragma once

#include <auth/auth-errors.hxx>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/session/repositories/change-outbox/change-outbox-repository.hxx>
#include <sync/auth-change-sink.hxx>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class NatsBus;

class AuthActionSink : public AuthChangeSink
{
public:
  struct Config
  {
    int retryMs{500};
    std::string actionSubject;
    std::string streamName;
  };

  AuthActionSink(std::shared_ptr<NatsBus> bus, Config config);
  ~AuthActionSink() override;
  AuthActionSink(const AuthActionSink&) = delete;
  AuthActionSink& operator=(const AuthActionSink&) = delete;

  [[nodiscard]] drogon::Task<void>
  publishAction(const AuthActionPublishInput& input) const override;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  static constexpr std::size_t kMaxPayloadBytes = std::size_t{256} * 1024;

private:
  [[nodiscard]] drogon::Task<void>
  enqueueAction(std::string payloadJson, drogon::orm::DbClient* client) const;
  [[nodiscard]] bool ensureStream() const;
  void flushLoop();
  bool flush(const ChangeOutboxRow& row);

  std::shared_ptr<NatsBus> bus_;
  ChangeOutboxRepository outbox_;
  const Config config_;
  const std::string actionSubject_;
  const std::string stream_;
  int64_t nextPurgeMs_{0};
  std::atomic<bool> streamReady_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> workerStarted_{false};
  std::atomic<bool> exited_{false};
  std::mutex wakeMutex_;
  mutable std::condition_variable wake_;
  std::thread worker_;
};
