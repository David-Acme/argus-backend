#pragma once

#include "outbox-backoff.hxx"
#include "outbox-repository.hxx"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <errors/error-definition.hxx>
#include <functional>
#include <memory>
#include <runtime/wake-signal.hxx>
#include <sqlite/transaction.hxx>
#include <string>
#include <string_view>
#include <thread>

class NatsBus;

namespace outbox
{

inline constexpr std::size_t kMaxPayloadBytes = std::size_t{256} * 1024;

struct OutboxRetention
{
  int64_t keepSentMs{0};
  int64_t purgeEveryMs{0};
  int64_t purgeRetryMs{0};
};

struct OutboxConfig
{
  std::string label;
  ClientAccessor client;
  std::string defaultSubject;
  std::string legacyIdPrefix;
  ErrorDefinition refusal;
  std::function<bool(NatsBus&)> ensureStreams;
  OutboxTiming timing;
  OutboxRetention retention;
};

struct OutboxRecordInput
{
  std::string eventId;
  std::string subject;
  std::string payload;
  drogon::orm::DbClient* client{nullptr};
};

struct OutboxAppendInput
{
  std::string_view idPrefix;
  std::string subject;
  std::string payload;
  drogon::orm::DbClient* client{nullptr};
};

class TransactionalOutbox
{
public:
  TransactionalOutbox(std::shared_ptr<NatsBus> bus, OutboxConfig config);
  ~TransactionalOutbox();
  TransactionalOutbox(const TransactionalOutbox&) = delete;
  TransactionalOutbox& operator=(const TransactionalOutbox&) = delete;
  TransactionalOutbox(TransactionalOutbox&&) = delete;
  TransactionalOutbox& operator=(TransactionalOutbox&&) = delete;

  [[nodiscard]] drogon::Task<OutboxDisposition>
  record(OutboxRecordInput input) const;

  [[nodiscard]] drogon::Task<void> append(OutboxAppendInput input) const;

  [[nodiscard]] bool migrateSchema() const;

  void reconcile();
  void requestStop();
  [[nodiscard]] bool drained() const;

  [[nodiscard]] const OutboxRepository& repository() const
  {
    return repository_;
  }

private:
  void refuseOversized(std::string_view what, std::size_t bytes) const;
  [[nodiscard]] bool ensureStreams();
  [[nodiscard]] bool flush(const OutboxRow& row);
  void purgeSettled();
  void flushLoop();

  std::shared_ptr<NatsBus> bus_;
  const OutboxConfig config_;
  OutboxRepository repository_;
  int64_t nextPurgeMs_{0};
  std::atomic<bool> streamReady_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> workerStarted_{false};
  std::atomic<bool> exited_{false};
  mutable WakeSignal wake_;
  db_transaction::CommitObserver commitObserver_{[this] { wake_.notify(); }};
  std::thread worker_;
};

}
