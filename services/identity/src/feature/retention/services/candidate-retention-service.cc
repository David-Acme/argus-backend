#include "candidate-retention-service.hxx"

#include <drogon/drogon.h>
#include <shared/services/face/face-service.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/identity-change-sink.hxx>
#include <sync/socket-emit-dto.hxx>
#include <runtime/blocking-task.hxx>

#include <chrono>
#include <ctime>
#include <exception>

namespace
{

constexpr int64_t kSecondsPerDay = 86400;
constexpr int64_t kBatch = 200;
constexpr int kMaxBatchesPerSweep = 25;
constexpr double kFirstSweepSeconds = 300.0;
constexpr double kSweepIntervalSeconds = 6.0 * 3600.0;

}

CandidateRetentionService::~CandidateRetentionService()
{
  stop();
}

void CandidateRetentionService::start()
{
  auto* loop = drogon::app().getLoop();
  firstTimer_ = loop->runAfter(kFirstSweepSeconds, [this] { launch(); });
  timer_ = loop->runEvery(kSweepIntervalSeconds, [this] { launch(); });
}

void CandidateRetentionService::stop()
{
  auto* loop = drogon::app().getLoop();
  for (auto* timer : {&firstTimer_, &timer_}) {
    if (timer->has_value())
      loop->invalidateTimer(**timer);
    timer->reset();
  }
}

void CandidateRetentionService::launch()
{
  if (running_.exchange(true))
    return;
  drogon::async_run([this]() -> drogon::Task<void> {
    try {
      const size_t retired = co_await sweep(std::time(nullptr));
      if (retired > 0)
        LOG_INFO << "Candidate retention: retired " << retired
                 << " unnamed visitor(s)";
    }
    catch (const std::exception& e) {
      LOG_WARN << "Candidate retention: sweep failed: " << e.what();
    }
    running_.store(false);
  });
}

drogon::Task<int64_t> CandidateRetentionService::cutoffAt(int64_t now) const
{
  if (!(co_await privacyGate_.household()).visitorRecognition)
    co_return now + 1;
  const auto setting = co_await settingRepository_.find();
  co_return now - setting.unnamedRetentionDays * kSecondsPerDay;
}

drogon::Task<size_t> CandidateRetentionService::sweep(int64_t now)
{
  const int64_t cutoff = co_await cutoffAt(now);
  size_t total = 0;
  for (int batch = 0; batch < kMaxBatchesPerSweep; ++batch) {
    const size_t retired = co_await retireBatch(cutoff);
    total += retired;
    if (retired < static_cast<size_t>(kBatch))
      break;
  }
  co_return total;
}

drogon::Task<size_t> CandidateRetentionService::retireBatch(int64_t cutoff)
{
  std::shared_ptr<drogon::orm::Transaction> transaction;
  std::vector<RetiredCandidate> retired;
  RetiredBiometrics purged;
  try {
    transaction = co_await db_transaction::begin(DbService::identityClient());
    retired = co_await repository_.retireStale(
        {.cutoff = cutoff, .limit = kBatch, .client = transaction.get()});
    if (retired.empty()) {
      db_transaction::rollback(transaction);
      co_return 0;
    }
    purged = co_await repository_.purgeBiometrics(retired, transaction.get());
    if (const auto* sink = identity_change::getSink()) {
      for (const auto& candidate : retired) {
        SocketEmitDto emit;
        emit.operation = SyncOperation::Delete;
        emit.option = TableName::Person;
        emit.obj["id"] = static_cast<Json::Int64>(candidate.id);
        emit.obj["deletedAt"] = static_cast<Json::Int64>(candidate.deletedAt);
        co_await sink->emitModule({.table = TableName::Person,
                                   .body = emit,
                                   .client = transaction.get()});
      }
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw std::runtime_error("the retention batch did not commit");
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  co_await BlockingTask<void>([ids = std::move(purged.embeddingIds)] {
    FaceService::instance().faceDb().removeEmbeddings(ids);
  });
  for (const auto& key : purged.cropKeys)
    co_await cropStore_.remove(key);
  co_return retired.size();
}
